
// ntfs.c — Real NTFS USN journal scan + tail
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <winioctl.h>
#include <wchar.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "anything.h"
#include "util.h"

// Minimal FRN map (open addressing)

typedef struct FrnEntry {
    uint64_t frn;
    uint64_t parent;
    wchar_t* name; // allocated
    uint32_t attrs;
} FrnEntry;

typedef struct FrnMap {
    FrnEntry* slots;
    size_t cap;
    size_t count;
} FrnMap;

static uint64_t frn_hash(uint64_t x){
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33;
    return x;
}
static void frnmap_init(FrnMap* m, size_t cap){
    m->cap = 1; while(m->cap < cap*2) m->cap <<= 1;
    m->slots = (FrnEntry*)calloc(m->cap, sizeof(FrnEntry));
    m->count = 0;
}
static void frnmap_free(FrnMap* m){
    if(!m->slots) return;
    for(size_t i=0;i<m->cap;i++){ if(m->slots[i].frn) free(m->slots[i].name); }
    free(m->slots); m->slots=NULL; m->cap=m->count=0;
}
static void frnmap_rehash_into(FrnMap* dst, const FrnMap* src){
    for(size_t i=0;i<src->cap;i++){
        if(!src->slots[i].frn) continue;
        FrnEntry e = src->slots[i];
        size_t j = (size_t)(frn_hash(e.frn) & (dst->cap-1));
        while(dst->slots[j].frn){ j=(j+1)&(dst->cap-1); }
        dst->slots[j] = e;
    }
}
static BOOL frnmap_resize(FrnMap* m){
    size_t newcap = m->cap ? m->cap*2 : 1024;
    FrnEntry* newslots = (FrnEntry*)calloc(newcap, sizeof(FrnEntry));
    if(!newslots) return FALSE;
    FrnMap tmp = {0};
    tmp.slots=newslots; tmp.cap=newcap; tmp.count=m->count;
    frnmap_rehash_into(&tmp, m);
    free(m->slots);
    m->slots = tmp.slots;
    m->cap = tmp.cap;
    return TRUE;
}
static FrnEntry* frnmap_put(FrnMap* m, uint64_t frn, uint64_t parent, const wchar_t* name, uint32_t attrs){
    if(m->count*2 >= m->cap){ if(!frnmap_resize(m)) return NULL; }
    size_t i = (size_t)(frn_hash(frn) & (m->cap-1));
    while(m->slots[i].frn && m->slots[i].frn != frn){ i=(i+1)&(m->cap-1); }
    if(!m->slots[i].frn){ m->count++; }
    m->slots[i].frn = frn; m->slots[i].parent = parent; m->slots[i].attrs=attrs;
    if(m->slots[i].name) free(m->slots[i].name);
    size_t n = wcslen(name);
    m->slots[i].name = (wchar_t*)malloc((n+1)*sizeof(wchar_t));
    wcscpy_s(m->slots[i].name, n+1, name);
    return &m->slots[i];
}
static FrnEntry* frnmap_get(FrnMap* m, uint64_t frn){
    size_t i = (size_t)(frn_hash(frn) & (m->cap-1));
    while(m->slots[i].frn){
        if(m->slots[i].frn==frn) return &m->slots[i];
        i=(i+1)&(m->cap-1);
    }
    return NULL;
}
    return NULL;
}

typedef struct USNScanner {
    HANDLE hVol;
    MPMCQueue* outq;
    HANDLE cancel;
    wchar_t volRoot[8]; // e.g., L"C:\\"
    wchar_t volPrefix[8]; // e.g., L"\\\\.\\C:" or root path for path building "C:\"
    FrnMap map;
    HANDLE thread;
} USNScanner;

static BOOL volume_from_root(const wchar_t* root, wchar_t* volprefix, size_t cch){
    if(!root || wcslen(root)<2 || root[1]!=L':') return FALSE;
    return swprintf(volprefix, cch, L"\\\\.\\%c:", root[0])>0;
}

// Build full path from FRN by walking parents in the map
static BOOL frn_build_path(FrnMap* fm, uint64_t frn, wchar_t* full, size_t cch){
    wchar_t temp[MAX_LONG_PATH]; temp[0]=0;
    wchar_t seg[512];
    size_t pos = 0;
    uint64_t cur = frn;
    int guard=0;
    while(cur && guard++ < 4096){
        FrnEntry* e = frnmap_get(fm, cur);
        if(!e) break;
        // prepend segment
        swprintf(seg, 512, L"\\%s", e->name);
        size_t sl = wcslen(seg);
        if(sl + pos >= MAX_LONG_PATH-4) return FALSE;
        memmove(temp+sl, temp, (pos+1)*sizeof(wchar_t));
        memcpy(temp, seg, sl*sizeof(wchar_t));
        pos += sl;
        cur = e->parent;
    }
    // temp begins with \Dir\Sub\Name
    if(wcslen(temp)==0) return FALSE;
    if(wcslen(temp)+3 >= cch) return FALSE;
    // Strip the last segment into parent and name: caller does this separately; here we return full path root + temp
    wcscpy_s(full, cch, L"");
    return wcscpy_s(full, cch, temp)==0;
}

static DWORD WINAPI usn_thread(void* p){
    USNScanner* s = (USNScanner*)p;
    // Enumerate MFT via FSCTL_ENUM_USN_DATA
    BYTE* buf = (BYTE*)VirtualAlloc(NULL, 16*1024*1024, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    if(!buf) return 1;
    MFT_ENUM_DATA_V1 med = {0};
    med.StartFileReferenceNumber = 0;
    med.LowUsn = 0;
    med.HighUsn = MAXLONGLONG;
    DWORD bytes;
    // first, fill the FRN map
    frnmap_init(&s->map, 1<<20);
    for(;;){
        if(WaitForSingleObject(s->cancel, 0)==WAIT_OBJECT_0) break;
        if(!DeviceIoControl(s->hVol, FSCTL_ENUM_USN_DATA, &med, sizeof(med), buf, 16*1024*1024, &bytes, NULL)){
            DWORD e = GetLastError(); if(e==ERROR_HANDLE_EOF) break; else { break; }
        }
        DWORD_PTR pRec = (DWORD_PTR)buf + sizeof(USN);
        while(pRec + sizeof(USN_RECORD_V2) <= (DWORD_PTR)buf + bytes){
            USN_RECORD_V2* r = (USN_RECORD_V2*)pRec;
            if(r->RecordLength < sizeof(USN_RECORD_V2)) break;
            wchar_t name[MAX_PATH];
            wcsncpy_s(name, MAX_PATH, (const wchar_t*)((BYTE*)r + r->FileNameOffset), r->FileNameLength/2);
            frnmap_put(&s->map, r->FileReferenceNumber, r->ParentFileReferenceNumber, name, r->FileAttributes);
            pRec += r->RecordLength;
        }
        med.StartFileReferenceNumber = *(USN*)buf;
    }
    // Second pass: emit work items by walking the map
    for(size_t i=0;i<s->map.cap;i++){
        if(!s->map.slots[i].frn) continue;
        FrnEntry* e = &s->map.slots[i];
        // Build full path from parent chain
        wchar_t rel[MAX_LONG_PATH];
        if(!frn_build_path(&s->map, e->parent, rel, MAX_LONG_PATH)){
            // If parent missing, skip
            continue;
        }
        // rel begins with \Dir\Sub; build absolute parent path: e.g., "C:\Dir\Sub"
        wchar_t parent[MAX_LONG_PATH];
        swprintf(parent, MAX_LONG_PATH, L"%c:%s", s->volRoot[0], rel);
        DbWorkItem* wi = (DbWorkItem*)_aligned_malloc(sizeof(DbWorkItem), CACHE_LINE_SIZE);
        wcscpy_s(wi->parent_path, MAX_LONG_PATH, parent);
        wcscpy_s(wi->name, MAX_PATH, e->name);
        // stat for times/size/attrs
        wchar_t full[MAX_LONG_PATH];
        path_join(full, MAX_LONG_PATH, parent, e->name);
        uint32_t attrs=0; uint64_t sz=0, ct=0, mt=0, at=0;
        get_file_info_basic(full, &attrs, &sz, &ct, &mt, &at);
        wi->attributes = attrs?attrs:e->attrs;
        wi->file_size = sz;
        wi->creation_time=ct; wi->modified_time=mt; wi->access_time=at;
        while(!MPMC_Push(s->outq, wi)) { SwitchToThread(); }
    }
    VirtualFree(buf,0,MEM_RELEASE);
    return 0;
}

NTFSScanner* NTFSScanner_Start(const wchar_t* volumeRoot, int threads, MPMCQueue* outQueue, HANDLE cancelEvent){
    (void)threads;
    USNScanner* s = (USNScanner*)calloc(1,sizeof(USNScanner));
    s->outq = outQueue; s->cancel = cancelEvent;
    wcscpy_s(s->volRoot, 8, volumeRoot);
    volume_from_root(volumeRoot, s->volPrefix, 8);
    s->hVol = CreateFileW(s->volPrefix, GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if(s->hVol==INVALID_HANDLE_VALUE){ free(s); return NULL; }
    s->thread = CreateThread(NULL,0,usn_thread,s,0,NULL);
    return (NTFSScanner*)s;
}
void NTFSScanner_Wait(NTFSScanner* s_){
    USNScanner* s = (USNScanner*)s_;
    if(!s) return;
    WaitForSingleObject(s->thread, INFINITE);
}
void NTFSScanner_Free(NTFSScanner* s_){
    USNScanner* s = (USNScanner*)s_;
    if(!s) return;
    CloseHandle(s->thread);
    if(s->hVol && s->hVol!=INVALID_HANDLE_VALUE) CloseHandle(s->hVol);
    frnmap_free(&s->map);
    free(s);
}

// USN tailer: read new records and emit updates
typedef struct TailCtx {
    HANDLE hVol;
    HANDLE cancel;
    MPMCQueue* outq;
    wchar_t root[8];
    HANDLE thread;
} TailCtx;

static DWORD WINAPI tail_thread(void* p){
    TailCtx* t = (TailCtx*)p;
    DWORD bytes=0;
    BYTE* buf = (BYTE*)VirtualAlloc(NULL, 1024*1024, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    if(!buf) return 0;
    USN_JOURNAL_DATA_V0 jd={0};
    if(!DeviceIoControl(t->hVol, FSCTL_QUERY_USN_JOURNAL, NULL,0, &jd, sizeof(jd), &bytes, NULL)){
        VirtualFree(buf,0,MEM_RELEASE); return 0;
    }
    READ_USN_JOURNAL_DATA_V0 readData={0};
    readData.UsnJournalID = jd.UsnJournalID;
    readData.StartUsn = jd.NextUsn;
    readData.ReasonMask = 0xFFFFFFFF;
    readData.BytesToWaitFor = 0; // poll
    readData.Timeout = 0;
    for(;;){
        if(WaitForSingleObject(t->cancel, 0)==WAIT_OBJECT_0) break;
        if(!DeviceIoControl(t->hVol, FSCTL_READ_USN_JOURNAL, &readData, sizeof(readData), buf, 1024*1024, &bytes, NULL)){
            Sleep(50); continue;
        }
        DWORD_PTR pRec = (DWORD_PTR)buf + sizeof(USN);
        while(pRec + sizeof(USN_RECORD_V2) <= (DWORD_PTR)buf + bytes){
            USN_RECORD_V2* r = (USN_RECORD_V2*)pRec;
            if(r->RecordLength < sizeof(USN_RECORD_V2)) break;
            // Build parent path best-effort: we don't maintain a full FRN map here; do a stat to reconstruct
            wchar_t name[MAX_PATH];
            wcsncpy_s(name, MAX_PATH, (const wchar_t*)((BYTE*)r + r->FileNameOffset), r->FileNameLength/2);
            // We only have parent FRN, not path; fall back to enumerating full path via GetFinalPathNameByHandle on the file handle if possible
            // Open by ID: requires FILE_ID_DESCRIPTOR
            HANDLE hFile = INVALID_HANDLE_VALUE;
            FILE_ID_DESCRIPTOR fid = {0}; fid.dwSize=sizeof(fid); fid.Type = FileIdType; fid.FileId.QuadPart = r->FileReferenceNumber;
            hFile = OpenFileById(t->hVol, &fid, FILE_READ_ATTRIBUTES, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, NULL, 0);
            if(hFile!=INVALID_HANDLE_VALUE){
                wchar_t full[MAX_LONG_PATH]; DWORD got = GetFinalPathNameByHandleW(hFile, full, MAX_LONG_PATH, FILE_NAME_NORMALIZED);
                CloseHandle(hFile);
                if(got>0 && got<MAX_LONG_PATH){
                    // full is \\?\C:\Dir\Name — split into parent/name
                    wchar_t parent[MAX_LONG_PATH]; wcscpy_s(parent, MAX_LONG_PATH, full);
                    if(wcsncmp(parent, L"\\\\?\\", 4)==0) { memmove(parent, parent+4, (wcslen(parent)-3)*sizeof(wchar_t)); }
                    wchar_t* p = wcsrchr(parent, L'\\'); if(p){ *p=0; }
                    DbWorkItem* wi = (DbWorkItem*)_aligned_malloc(sizeof(DbWorkItem), CACHE_LINE_SIZE);
                    wcscpy_s(wi->parent_path, MAX_LONG_PATH, parent);
                    wcscpy_s(wi->name, MAX_PATH, name);
                    uint32_t attrs=0; uint64_t sz=0, ct=0, mt=0, at=0;
                    wchar_t fn[MAX_LONG_PATH]; swprintf(fn, MAX_LONG_PATH, L"%s\\%s", parent, name);
                    get_file_info_basic(fn, &attrs, &sz, &ct, &mt, &at);
                    wi->attributes = attrs?attrs: r->FileAttributes;
                    wi->file_size = sz; wi->creation_time=ct; wi->modified_time=mt; wi->access_time=at;
                    while(!MPMC_Push(t->outq, wi)) { SwitchToThread(); }
                }
            }
            pRec += r->RecordLength;
        }
        readData.StartUsn = *(USN*)buf;
    }
    VirtualFree(buf,0,MEM_RELEASE);
    return 0;
}

HANDLE StartUSNTailer(const wchar_t* volumeRoot, MPMCQueue* outQueue, HANDLE cancelEvent){
    wchar_t volprefix[8];
    if(!volume_from_root(volumeRoot, volprefix, 8)) return NULL;
    HANDLE hVol = CreateFileW(volprefix, GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if(hVol==INVALID_HANDLE_VALUE) return NULL;
    TailCtx* t = (TailCtx*)calloc(1,sizeof(TailCtx));
    t->hVol=hVol; t->cancel=cancelEvent; t->outq=outQueue;
    wcscpy_s(t->root, 8, volumeRoot);
    t->thread = CreateThread(NULL,0,tail_thread,t,0,NULL);
    return t->thread;
}
