// ntfs.c - NTFS USN Journal scanner for ANYTHING (fast path)
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <process.h>

#include "anything.h"

#ifndef USN_PAGE_SIZE
#define USN_PAGE_SIZE (64*1024*1024) // 64MB buffer
#endif

typedef struct NtfsItem {
    uint64_t frn;
    uint64_t parent_frn;
    uint32_t attributes;
    BOOL     is_dir;
    WCHAR*   name; // allocated
} NtfsItem;

typedef struct FrnMap {
    uint64_t* keys;
    uint32_t* vals; // index into items array
    uint32_t  cap;
    uint32_t  count;
} FrnMap;

static uint32_t next_pow2_u32(uint32_t v) {
    if (v < 2) return 2;
    v--; v|=v>>1; v|=v>>2; v|=v>>4; v|=v>>8; v|=v>>16; v++;
    return v;
}

static void frnmap_init(FrnMap* m, uint32_t expected) {
    m->cap = next_pow2_u32(expected * 2);
    m->count = 0;
    m->keys = (uint64_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, m->cap * sizeof(uint64_t));
    m->vals = (uint32_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, m->cap * sizeof(uint32_t));
}

static void frnmap_free(FrnMap* m) {
    HeapFree(GetProcessHeap(), 0, m->keys);
    HeapFree(GetProcessHeap(), 0, m->vals);
    ZeroMemory(m, sizeof(*m));
}

static uint32_t frn_hash(uint64_t k) {
    k ^= k >> 33; k *= 0xff51afd7ed558ccdULL; k ^= k >> 33; k *= 0xc4ceb9fe1a85ec53ULL; k ^= k >> 33;
    return (uint32_t)k;
}

static void frnmap_put(FrnMap* m, uint64_t key, uint32_t val) {
    uint32_t mask = m->cap - 1;
    uint32_t i = frn_hash(key) & mask;
    for (;;) {
        if (m->keys[i] == 0) {
            m->keys[i] = key;
            m->vals[i] = val;
            m->count++;
            return;
        }
        if (m->keys[i] == key) { m->vals[i] = val; return; }
        i = (i+1) & mask;
    }
}

static BOOL frnmap_get(const FrnMap* m, uint64_t key, uint32_t* out) {
    uint32_t mask = m->cap - 1;
    uint32_t i = frn_hash(key) & mask;
    for (;;) {
        if (m->keys[i] == 0) return FALSE;
        if (m->keys[i] == key) { *out = m->vals[i]; return TRUE; }
        i = (i+1) & mask;
    }
}

// --- Scanner object --------------------------------------------------------

typedef struct {
    HANDLE   volume;
    WCHAR    root_letter; // 'C'
    WCHAR    root_path[8]; // L"C:\\"
    NtfsItem* items;
    uint32_t  count;
    uint32_t  capacity;
    MPMCQueue* out_q;
    HANDLE cancel_event;
    FrnMap map;
    HANDLE* workers;
    int worker_count;
} NTFSScannerImpl;

static void items_push(NTFSScannerImpl* s, const NtfsItem* it) {
    if (s->count == s->capacity) {
        uint32_t ncap = s->capacity? s->capacity*2 : 32768;
        s->items = (NtfsItem*)HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, s->items, ncap * sizeof(NtfsItem));
        if (!s->items) { fprintf(stderr, "Out of mem items\n"); ExitProcess(4); }
        s->capacity = ncap;
    }
    s->items[s->count++] = *it;
}

static HANDLE open_volume_from_root(const WCHAR* volumeRoot, WCHAR* out_letter) {
    // Expect X:\ or \\?\X:\
    WCHAR letter=0;
    if (wcslen(volumeRoot) >= 2 && volumeRoot[1] == L':') {
        letter = towupper(volumeRoot[0]);
    } else if (wcsncmp(volumeRoot, L"\\\\?\\", 4)==0 && wcslen(volumeRoot) >= 6 && volumeRoot[5]==L':') {
        letter = towupper(volumeRoot[4]);
    } else {
        return INVALID_HANDLE_VALUE;
    }
    WCHAR volpath[8];
    swprintf_s(volpath, 8, L"\\\\.\\%c:", letter);
    HANDLE h = CreateFileW(volpath, GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h != INVALID_HANDLE_VALUE) *out_letter = letter;
    return h;
}

static void ensure_root_path(NTFSScannerImpl* s) {
    swprintf_s(s->root_path, 8, L"%c:\\", s->root_letter);
}

// --- USN enumeration -------------------------------------------------------

static BOOL enumerate_usn(NTFSScannerImpl* s) {
    BYTE* buffer = (BYTE*)VirtualAlloc(NULL, USN_PAGE_SIZE, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    if (!buffer) buffer = (BYTE*)HeapAlloc(GetProcessHeap(), 0, USN_PAGE_SIZE/2);
    if (!buffer) return FALSE;

    MFT_ENUM_DATA_V0 med = {0};
    med.StartFileReferenceNumber = 0;
    med.LowUsn  = 0;
    med.HighUsn = (USN)-1;

    DWORD bytes = 0;
    USN usn = 0;

    for (;;) {
        BOOL ok = DeviceIoControl(s->volume, FSCTL_ENUM_USN_DATA,
                                  &med, sizeof(med),
                                  buffer, USN_PAGE_SIZE,
                                  &bytes, NULL);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_HANDLE_EOF) break;
            break;
        }
        BYTE* p = buffer + sizeof(USN);
        BYTE* end = buffer + bytes;
        while (p + sizeof(USN_RECORD_V2) <= end) {
            USN_RECORD_V2* r = (USN_RECORD_V2*)p;
            if (r->RecordLength == 0) break;

            NtfsItem it = {0};
            it.frn = r->FileReferenceNumber;
            it.parent_frn = r->ParentFileReferenceNumber;
            it.attributes = r->FileAttributes;
            it.is_dir = (r->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

            int nameLen = r->FileNameLength / sizeof(WCHAR);
            it.name = (WCHAR*)HeapAlloc(GetProcessHeap(), 0, (nameLen+1)*sizeof(WCHAR));
            if (it.name) {
                wcsncpy_s(it.name, nameLen+1, (WCHAR*)((BYTE*)r + r->FileNameOffset), nameLen);
            }
            items_push(s, &it);

            p += r->RecordLength;
        }
        usn = *(USN*)buffer;
        med.StartFileReferenceNumber = *(uint64_t*)buffer;
    }

    if (buffer) {
        if (VirtualFree(buffer, 0, MEM_RELEASE)==0) HeapFree(GetProcessHeap(), 0, buffer);
    }

    return s->count > 0;
}

// --- Path reconstruction and emission --------------------------------------

typedef struct {
    NTFSScannerImpl* s;
    uint32_t start;
    uint32_t end;
} PathChunk;

static BOOL build_parent_path(NTFSScannerImpl* s, uint32_t idx, WCHAR* out, size_t outcch) {
    // Build path excluding the item's own name.
    // Follow parent_frn chain until reaching the volume root (parent_frn == frn of root or 0).
    const NtfsItem* cur = &s->items[idx];
    // Accumulate segments in temp array (at most 1024 depth).
    const WCHAR* segs[1024];
    int seg_count = 0;

    uint64_t parent = cur->parent_frn;
    uint32_t ix = 0;
    while (parent && seg_count < 1024) {
        if (!frnmap_get(&s->map, parent, &ix)) {
            // Unknown parent (maybe deleted): assume root
            break;
        }
        const NtfsItem* pit = &s->items[ix];
        // stop at volume root (its parent can be itself or 0)
        if (pit->parent_frn == pit->frn || pit->parent_frn == 0) {
            // reached root directory for letter
            break;
        }
        segs[seg_count++] = pit->name;
        parent = pit->parent_frn;
    }

    // Compose: X:\ + segs reversed
    size_t len = 0;
    WCHAR tmp[MAX_LONG_PATH];
    tmp[0]=0;
    len += swprintf_s(tmp+len, MAX_LONG_PATH-len, L"%s", s->root_path);
    for (int i=seg_count-1; i>=0; --i) {
        if (len+1 >= MAX_LONG_PATH) break;
        tmp[len++] = L'\\';
        tmp[len] = 0;
        size_t seglen = wcslen(segs[i]);
        wcsncat_s(tmp, MAX_LONG_PATH, segs[i], seglen);
        len = wcslen(tmp);
    }
    wcsncpy_s(out, outcch, tmp, _TRUNCATE);
    return TRUE;
}

static void emit_by_index(NTFSScannerImpl* s, uint32_t idx) {
    const NtfsItem* it = &s->items[idx];

    WCHAR parent_path[MAX_LONG_PATH];
    build_parent_path(s, idx, parent_path, MAX_LONG_PATH);

    DbWorkItem* wi = (DbWorkItem*)_aligned_malloc(sizeof(DbWorkItem), CACHE_LINE_SIZE);
    if (!wi) return;
    ZeroMemory(wi, sizeof(*wi));

    wi->type = it->is_dir ? DB_REC_DIR : DB_REC_FILE;
    wcsncpy_s(wi->parent_path, MAX_LONG_PATH, parent_path, _TRUNCATE);
    wcsncpy_s(wi->name, MAX_PATH, it->name ? it->name : L"", _TRUNCATE);
    wi->attributes = it->attributes;

    // Query file attributes + times for the full path
    WCHAR full[MAX_LONG_PATH];
    wcsncpy_s(full, MAX_LONG_PATH, parent_path, _TRUNCATE);
    size_t len = wcslen(full);
    if (len && full[len-1] != L'\\') wcscat_s(full, MAX_LONG_PATH, L"\\");
    wcscat_s(full, MAX_LONG_PATH, it->name ? it->name : L"");

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(full, GetFileExInfoStandard, &fad)) {
        ULARGE_INTEGER szz; szz.LowPart=fad.nFileSizeLow; szz.HighPart=fad.nFileSizeHigh;
        wi->file_size = szz.QuadPart;
        ULARGE_INTEGER u;
        u.LowPart=fad.ftCreationTime.dwLowDateTime; u.HighPart=fad.ftCreationTime.dwHighDateTime;
        wi->creation_time = u.QuadPart;
        u.LowPart=fad.ftLastWriteTime.dwLowDateTime; u.HighPart=fad.ftLastWriteTime.dwHighDateTime;
        wi->modified_time = u.QuadPart;
        u.LowPart=fad.ftLastAccessTime.dwLowDateTime; u.HighPart=fad.ftLastAccessTime.dwHighDateTime;
        wi->access_time = u.QuadPart;
        wi->attributes = fad.dwFileAttributes;
    }

    while (!MPMC_Push(s->out_q, wi)) {
        if (WaitForSingleObject(s->cancel_event, 0) == WAIT_OBJECT_0) {
            _aligned_free(wi);
            return;
        }
        SwitchToThread();
    }
}

static unsigned __stdcall path_worker(void* param) {
    PathChunk* c = (PathChunk*)param;
    NTFSScannerImpl* s = c->s;
    for (uint32_t i=c->start; i<c->end; ++i) {
        if (WaitForSingleObject(s->cancel_event, 0) == WAIT_OBJECT_0) break;
        emit_by_index(s, i);
    }
    return 0;
}

// --- Public API ------------------------------------------------------------

NTFSScanner* NTFSScanner_Start(const WCHAR* volumeRoot, int threads, MPMCQueue* outQueue, HANDLE cancelEvent) {
    NTFSScannerImpl* s = (NTFSScannerImpl*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(NTFSScannerImpl));
    if (!s) return NULL;

    s->out_q = outQueue;
    s->cancel_event = cancelEvent;
    s->volume = open_volume_from_root(volumeRoot, &s->root_letter);
    if (s->volume == INVALID_HANDLE_VALUE) {
        HeapFree(GetProcessHeap(), 0, s);
        return NULL;
    }
    ensure_root_path(s);

    if (!enumerate_usn(s)) {
        CloseHandle(s->volume);
        HeapFree(GetProcessHeap(), 0, s);
        return NULL;
    }

    // Build FRN map
    frnmap_init(&s->map, s->count);
    for (uint32_t i=0;i<s->count;i++) {
        frnmap_put(&s->map, s->items[i].frn, i);
    }

    // Create path workers
    s->worker_count = (threads > 0 && threads <= MAX_THREADS) ? threads : 1;
    s->workers = (HANDLE*)HeapAlloc(GetProcessHeap(), 0, sizeof(HANDLE)*s->worker_count);
    if (!s->workers) {
        frnmap_free(&s->map);
        CloseHandle(s->volume);
        HeapFree(GetProcessHeap(), 0, s);
        return NULL;
    }

    // Divide into contiguous chunks
    uint32_t chunk = (s->count + s->worker_count - 1) / s->worker_count;
    for (int i=0;i<s->worker_count;i++) {
        PathChunk* pc = (PathChunk*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(PathChunk));
        pc->s = s;
        pc->start = i*chunk;
        pc->end = pc->start + chunk;
        if (pc->end > s->count) pc->end = s->count;
        uintptr_t th = _beginthreadex(NULL, 0, path_worker, pc, 0, NULL);
        s->workers[i] = (HANDLE)th;
    }

    return (NTFSScanner*)s;
}

void NTFSScanner_Wait(NTFSScanner* scanner) {
    NTFSScannerImpl* s = (NTFSScannerImpl*)scanner;
    if (!s) return;
    WaitForMultipleObjects(s->worker_count, s->workers, TRUE, INFINITE);
}

void NTFSScanner_Free(NTFSScanner* scanner) {
    NTFSScannerImpl* s = (NTFSScannerImpl*)scanner;
    if (!s) return;
    for (int i=0;i<s->worker_count;i++) {
        if (s->workers[i]) CloseHandle(s->workers[i]);
    }
    HeapFree(GetProcessHeap(), 0, s->workers);
    for (uint32_t i=0;i<s->count;i++) {
        if (s->items[i].name) HeapFree(GetProcessHeap(), 0, s->items[i].name);
    }
    HeapFree(GetProcessHeap(), 0, s->items);
    frnmap_free(&s->map);
    if (s->volume != INVALID_HANDLE_VALUE) CloseHandle(s->volume);
    HeapFree(GetProcessHeap(), 0, s);
}
