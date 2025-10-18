
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
#include <process.h>
#include "anything.h"
#include "util.h"

typedef struct AdaptiveThreadPool {
    int min_threads;
    int max_threads;
    volatile int current_threads;
    HANDLE* threads;
    volatile LONG work_queue_size;
    LARGE_INTEGER last_adjustment;
    LPTHREAD_START_ROUTINE worker;
    void* worker_arg;
} AdaptiveThreadPool;

static void adjust_thread_count(AdaptiveThreadPool* pool){
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if(pool->work_queue_size > pool->current_threads * 10 &&
       pool->current_threads < pool->max_threads){
        HANDLE h = (HANDLE)_beginthreadex(NULL,0,pool->worker,pool->worker_arg,0,NULL);
        if(h){
            pool->threads[pool->current_threads++] = h;
        }
    }
    pool->last_adjustment = now;
}

static void pool_init(AdaptiveThreadPool* pool, int min_t, int max_t,
                      LPTHREAD_START_ROUTINE worker, void* arg){
    pool->min_threads = min_t;
    pool->max_threads = max_t;
    pool->current_threads = 0;
    pool->threads = (HANDLE*)calloc(max_t, sizeof(HANDLE));
    pool->worker = worker;
    pool->worker_arg = arg;
    pool->work_queue_size = 0;
    QueryPerformanceCounter(&pool->last_adjustment);
    for(int i=0;i<min_t;i++){
        HANDLE h = (HANDLE)_beginthreadex(NULL,0,worker,arg,0,NULL);
        if(h){ pool->threads[pool->current_threads++] = h; }
    }
}

static void pool_destroy(AdaptiveThreadPool* pool){
    if(!pool) return;
    for(int i=0;i<pool->current_threads;i++) CloseHandle(pool->threads[i]);
    free(pool->threads);
    pool->threads = NULL;
    pool->current_threads = 0;
}

// Minimal FRN map (open addressing)

typedef struct NameArena {
    wchar_t* base;
    size_t used;
    size_t cap;
    struct NameArena* next;
    struct NameArena* free_next;
    HANDLE file;
    HANDLE mapping;
    BOOL is_mmap;
    BOOL is_small;
} NameArena;

typedef struct FrnEntry {
    uint64_t frn;
    uint64_t parent;
    wchar_t* name; // from arena
    uint32_t attrs;
} FrnEntry;

typedef struct NameArenaPoolBlock NameArenaPoolBlock;

typedef struct FrnMap {
    FrnEntry* slots;
    size_t cap;
    size_t count;
    NameArena* arena;
    NameArena* current_small;
    size_t arena_total_bytes;
    size_t arena_limit_bytes;
    size_t mmap_total_bytes;
    size_t mmap_limit_bytes;
    size_t small_chunk_chars;
    NameArena* arena_free_list;
    NameArenaPoolBlock* arena_pool_blocks;
    BOOL streaming;
} FrnMap;

#define FRNMAP_STREAMING_SENTINEL ((FrnEntry*)(intptr_t)(-1))
#define FRN_ARENA_DEFAULT_CHUNK 4096
#define FRN_ARENA_HEAP_LIMIT_BYTES (size_t)(512ULL*1024ULL*1024ULL)
#define FRN_ARENA_MMAP_LIMIT_BYTES (size_t)(4ULL*1024ULL*1024ULL*1024ULL)
#define FRN_SMALL_NAME_THRESHOLD 32
#define FRN_SMALL_ARENA_CHARS (size_t)(20ULL*256ULL) // tuned for ~20 char average names
#define FRN_ARENA_POOL_BATCH 32
#define FRN_ARENA_ALIGN_CHARS 4
#define FRN_MMAP_MIN_CHARS (size_t)((64ULL*1024ULL)/sizeof(wchar_t))

typedef struct NameArenaPoolBlock {
    struct NameArenaPoolBlock* next;
    NameArena arenas[FRN_ARENA_POOL_BATCH];
} NameArenaPoolBlock;

static uint64_t frn_hash(uint64_t x){
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33;
    return x;
}

static size_t frn_align_chars(size_t n){
    size_t align = FRN_ARENA_ALIGN_CHARS;
    return (n + (align-1)) & ~(align-1);
}

static BOOL frnmap_grow_pool(FrnMap* m){
    NameArenaPoolBlock* block = (NameArenaPoolBlock*)calloc(1, sizeof(NameArenaPoolBlock));
    if(!block){
        return FALSE;
    }
    block->next = m->arena_pool_blocks;
    m->arena_pool_blocks = block;
    for(size_t i = 0; i < FRN_ARENA_POOL_BATCH; ++i){
        block->arenas[i].free_next = m->arena_free_list;
        m->arena_free_list = &block->arenas[i];
    }
    return TRUE;
}

static NameArena* frnmap_acquire_node(FrnMap* m){
    if(!m->arena_free_list){
        if(!frnmap_grow_pool(m)){
            return NULL;
        }
    }
    NameArena* arena = m->arena_free_list;
    m->arena_free_list = arena->free_next;
    memset(arena, 0, sizeof(NameArena));
    return arena;
}

static void frnmap_release_node(FrnMap* m, NameArena* arena){
    if(!arena){
        return;
    }
    arena->free_next = m->arena_free_list;
    m->arena_free_list = arena;
}
static void frnmap_init(FrnMap* m, size_t cap){
    m->cap = 1; while(m->cap < cap*2) m->cap <<= 1;
    m->slots = (FrnEntry*)calloc(m->cap, sizeof(FrnEntry));
    m->count = 0;
    m->arena = NULL;
    m->current_small = NULL;
    m->arena_total_bytes = 0;
    m->arena_limit_bytes = FRN_ARENA_HEAP_LIMIT_BYTES;
    m->mmap_total_bytes = 0;
    m->mmap_limit_bytes = FRN_ARENA_MMAP_LIMIT_BYTES;
    m->small_chunk_chars = frn_align_chars(FRN_SMALL_ARENA_CHARS);
    m->arena_free_list = NULL;
    m->arena_pool_blocks = NULL;
    m->streaming = FALSE;
}
static void frnmap_free(FrnMap* m){
    if(m->slots) free(m->slots);
    NameArena* a = m->arena;
    while(a){
        NameArena* next = a->next;
        if(a->is_mmap){
            if(a->base) UnmapViewOfFile(a->base);
            if(a->mapping) CloseHandle(a->mapping);
            if(a->file && a->file!=INVALID_HANDLE_VALUE) CloseHandle(a->file);
        } else {
            free(a->base);
        }
        a = next;
    }
    NameArenaPoolBlock* block = m->arena_pool_blocks;
    while(block){
        NameArenaPoolBlock* next_block = block->next;
        free(block);
        block = next_block;
    }
    m->slots = NULL;
    m->cap = 0;
    m->count = 0;
    m->arena = NULL;
    m->current_small = NULL;
    m->arena_total_bytes = 0;
    m->mmap_total_bytes = 0;
    m->arena_limit_bytes = FRN_ARENA_HEAP_LIMIT_BYTES;
    m->mmap_limit_bytes = FRN_ARENA_MMAP_LIMIT_BYTES;
    m->small_chunk_chars = frn_align_chars(FRN_SMALL_ARENA_CHARS);
    m->arena_free_list = NULL;
    m->arena_pool_blocks = NULL;
    m->streaming = FALSE;
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
static BOOL frnmap_create_mmap_arena(FrnMap* m, size_t cap, NameArena** out){
    size_t aligned = frn_align_chars(cap);
    size_t gran = FRN_MMAP_MIN_CHARS;
    if(aligned < gran){
        aligned = gran;
    } else if(aligned % gran){
        aligned += gran - (aligned % gran);
    }
    if(m->mmap_total_bytes + aligned * sizeof(wchar_t) > m->mmap_limit_bytes){
        m->streaming = TRUE;
        return FALSE;
    }
    wchar_t tempPath[MAX_PATH];
    wchar_t tempFile[MAX_PATH];
    if(!GetTempPathW(MAX_PATH, tempPath)){
        return FALSE;
    }
    if(!GetTempFileNameW(tempPath, L"afr", 0, tempFile)){
        return FALSE;
    }
    HANDLE file = CreateFileW(tempFile, GENERIC_READ|GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if(file==INVALID_HANDLE_VALUE){
        return FALSE;
    }
    ULONGLONG bytes = (ULONGLONG)aligned * sizeof(wchar_t);
    LARGE_INTEGER size;
    size.QuadPart = (LONGLONG)bytes;
    if(!SetFilePointerEx(file, size, NULL, FILE_BEGIN) || !SetEndOfFile(file)){
        CloseHandle(file);
        return FALSE;
    }
    DWORD sizeHigh = (DWORD)(bytes >> 32);
    DWORD sizeLow = (DWORD)(bytes & 0xFFFFFFFFUL);
    HANDLE mapping = CreateFileMappingW(file, NULL, PAGE_READWRITE, sizeHigh, sizeLow, NULL);
    if(!mapping){
        CloseHandle(file);
        return FALSE;
    }
    wchar_t* base = (wchar_t*)MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, (SIZE_T)(aligned * sizeof(wchar_t)));
    if(!base){
        CloseHandle(mapping);
        CloseHandle(file);
        return FALSE;
    }
    NameArena* arena = frnmap_acquire_node(m);
    if(!arena){
        UnmapViewOfFile(base);
        CloseHandle(mapping);
        CloseHandle(file);
        return FALSE;
    }
    arena->base = base;
    arena->used = 0;
    arena->cap = aligned;
    arena->file = file;
    arena->mapping = mapping;
    arena->is_mmap = TRUE;
    arena->is_small = FALSE;
    arena->free_next = NULL;
    *out = arena;
    m->mmap_total_bytes += aligned * sizeof(wchar_t);
    return TRUE;
}

static NameArena* frnmap_push_arena(FrnMap* m, size_t n, BOOL is_small){
    size_t need = frn_align_chars(n);
    if(is_small){
        size_t chunk = m->small_chunk_chars;
        if(chunk < need){
            chunk = need;
        }
        need = frn_align_chars(chunk);
    }
    size_t projected_bytes = m->arena_total_bytes + need * sizeof(wchar_t);
    if(is_small && projected_bytes > m->arena_limit_bytes){
        m->streaming = TRUE;
        return NULL;
    }
    NameArena* arena = NULL;
    BOOL use_mmap = (!is_small) && (projected_bytes > m->arena_limit_bytes);
    if(use_mmap){
        if(!frnmap_create_mmap_arena(m, need, &arena)){
            return NULL;
        }
    } else {
        arena = frnmap_acquire_node(m);
        if(!arena) return NULL;
        arena->base = (wchar_t*)malloc(need * sizeof(wchar_t));
        if(!arena->base){
            frnmap_release_node(m, arena);
            return NULL;
        }
        arena->used = 0;
        arena->cap = need;
        arena->file = NULL;
        arena->mapping = NULL;
        arena->is_mmap = FALSE;
        arena->is_small = is_small;
        arena->free_next = NULL;
        m->arena_total_bytes += need * sizeof(wchar_t);
    }
    arena->is_small = is_small;
    arena->next = m->arena;
    arena->free_next = NULL;
    m->arena = arena;
    return arena;
}

static wchar_t* frnmap_alloc_name(FrnMap* m, size_t n, BOOL is_small){
    if(m->streaming){
        return NULL;
    }
    size_t need = frn_align_chars(n);
    if(is_small){
        NameArena* arena = m->current_small;
        if(!arena || arena->used + need > arena->cap){
            arena = frnmap_push_arena(m, need, TRUE);
            if(!arena){
                return NULL;
            }
            m->current_small = arena;
        }
        wchar_t* ret = arena->base + arena->used;
        arena->used += need;
        return ret;
    }
    NameArena* arena = frnmap_push_arena(m, need, FALSE);
    if(!arena){
        return NULL;
    }
    arena->used = need;
    return arena->base;
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
    if(m->streaming){
        return FRNMAP_STREAMING_SENTINEL;
    }
    if(m->count*2 >= m->cap){
        if(!frnmap_resize(m)){
            return NULL;
        }
    }
    size_t i = (size_t)(frn_hash(frn) & (m->cap-1));
    while(m->slots[i].frn && m->slots[i].frn != frn){ i=(i+1)&(m->cap-1); }
    BOOL is_new = !m->slots[i].frn;
    size_t name_len = wcslen(name);
    size_t chars_with_null = name_len + 1;
    size_t padded = frn_align_chars(chars_with_null);
    BOOL use_small = name_len < FRN_SMALL_NAME_THRESHOLD;
    wchar_t* dst = frnmap_alloc_name(m, padded, use_small);
    if(!dst){
        if(is_new){
            m->slots[i].frn = 0;
        }
        if(m->streaming){
            return FRNMAP_STREAMING_SENTINEL;
        }
        return NULL;
    }
    if(is_new){
        m->count++;
    }
    m->slots[i].frn = frn;
    m->slots[i].parent = parent;
    m->slots[i].attrs = attrs;
    memcpy(dst, name, chars_with_null*sizeof(wchar_t));
    m->slots[i].name = dst;
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

typedef struct USNScanner {
    HANDLE hVol;
    MPMCQueue* outq;
    CancelToken* cancel;
    wchar_t volRoot[8]; // e.g., L"C:\\"
    wchar_t volPrefix[8]; // e.g., L"\\\\.\\C:" or root path for path building "C:\"
    FrnMap map;
    AdaptiveThreadPool pool;
    volatile LONG next_idx;
    int max_threads;
    HANDLE thread;
    BOOL streaming_mode;
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
    size_t guard = 0;
    uint64_t visited[4096];
    size_t visited_count = 0;
    while(cur && guard < 4096){
        for(size_t i = 0; i < visited_count; ++i){
            if(visited[i] == cur){
                return FALSE;
            }
        }
        if(visited_count >= sizeof(visited)/sizeof(visited[0])){
            return FALSE;
        }
        visited[visited_count++] = cur;
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
        guard++;
    }
    if(cur){
        return FALSE;
    }
    // temp begins with \Dir\Sub\Name
    if(wcslen(temp)==0) return FALSE;
    if(wcslen(temp)+3 >= cch) return FALSE;
    // Strip the last segment into parent and name: caller does this separately; here we return full path root + temp
    wcscpy_s(full, cch, L"");
    return wcscpy_s(full, cch, temp)==0;
}

static BOOL frn_resolve_parent_path(USNScanner* s, uint64_t parent_frn, wchar_t* parent, size_t cch){
    if(parent_frn == 0){
        return swprintf(parent, cch, L"%c:\\", s->volRoot[0])>0;
    }
    FrnEntry* e = frnmap_get(&s->map, parent_frn);
    if(e){
        wchar_t rel[MAX_LONG_PATH];
        if(!frn_build_path(&s->map, parent_frn, rel, MAX_LONG_PATH)){
            return FALSE;
        }
        return swprintf(parent, cch, L"%c:%s", s->volRoot[0], rel)>0;
    }
    FILE_ID_DESCRIPTOR pfid = {0};
    pfid.dwSize = sizeof(pfid);
    pfid.Type = FileIdType;
    pfid.FileId.QuadPart = parent_frn;
    HANDLE hPar = OpenFileById(s->hVol, &pfid, FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, NULL, 0);
    if(hPar == INVALID_HANDLE_VALUE){
        return FALSE;
    }
    BOOL ok = FALSE;
    wchar_t parent_full[MAX_LONG_PATH];
    DWORD got = GetFinalPathNameByHandleW(hPar, parent_full, MAX_LONG_PATH, FILE_NAME_NORMALIZED);
    CloseHandle(hPar);
    if(got>0 && got<MAX_LONG_PATH){
        if(wcsncmp(parent_full, L"\\?\", 4)==0){
            memmove(parent_full, parent_full+4, (wcslen(parent_full)-3)*sizeof(wchar_t));
        }
        ok = wcscpy_s(parent, cch, parent_full)==0;
    }
    return ok;
}

static BOOL frn_emit_direct(USNScanner* s, const USN_RECORD_V2* r){
    if(is_cancelled(s->cancel)){
        return FALSE;
    }
    wchar_t parent[MAX_LONG_PATH];
    if(!frn_resolve_parent_path(s, r->ParentFileReferenceNumber, parent, MAX_LONG_PATH)){
        return FALSE;
    }
    wchar_t name[MAX_PATH];
    wcsncpy_s(name, MAX_PATH, (const wchar_t*)((BYTE*)r + r->FileNameOffset), r->FileNameLength/2);
    DbWorkItem* wi = (DbWorkItem*)aligned_malloc(sizeof(DbWorkItem), CACHE_LINE_SIZE);
    if(!wi){
        return FALSE;
    }
    wi->content = NULL;
    wi->preview = NULL;
    wcscpy_s(wi->parent_path, MAX_LONG_PATH, parent);
    wcscpy_s(wi->name, MAX_PATH, name);
    wi->file_size = 0;
    wi->creation_time = wi->modified_time = wi->access_time = 0;
    wi->attributes = r->FileAttributes;
    wi->clone_id = 0;
    wi->stage = INDEX_NAMES_ONLY;
    wi->op = WI_ADD;
    while(!MPMC_Push(s->outq, wi)){
        if(is_cancelled(s->cancel)){
            aligned_free(wi);
            return FALSE;
        }
        SwitchToThread();
    }
    return TRUE;
}

static DWORD WINAPI map_emit_worker(void* p){
    USNScanner* s = (USNScanner*)p;
    for(;;){
        LONG idx = InterlockedIncrement(&s->next_idx) - 1;
        if(idx >= (LONG)s->map.cap) break;
        if(!s->map.slots[idx].frn){
            InterlockedDecrement(&s->pool.work_queue_size);
            continue;
        }
        FrnEntry* e = &s->map.slots[idx];
        wchar_t rel[MAX_LONG_PATH];
        if(!frn_build_path(&s->map, e->parent, rel, MAX_LONG_PATH)){
            InterlockedDecrement(&s->pool.work_queue_size);
            continue;
        }
        wchar_t parent[MAX_LONG_PATH];
        swprintf(parent, MAX_LONG_PATH, L"%c:%s", s->volRoot[0], rel);
        DbWorkItem* wi = (DbWorkItem*)aligned_malloc(sizeof(DbWorkItem), CACHE_LINE_SIZE);
        wi->content = NULL; wi->preview = NULL;
        wcscpy_s(wi->parent_path, MAX_LONG_PATH, parent);
        wcscpy_s(wi->name, MAX_PATH, e->name);
        wi->file_size = 0;
        wi->creation_time = wi->modified_time = wi->access_time = 0;
        wi->attributes = e->attrs;
        wi->clone_id = 0;
        wi->stage = INDEX_NAMES_ONLY;
        wi->op = WI_ADD;
        while(!MPMC_Push(s->outq, wi)) { SwitchToThread(); }
        InterlockedDecrement(&s->pool.work_queue_size);
        adjust_thread_count(&s->pool);
        if(is_cancelled(s->cancel)) break;
    }
    return 0;
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
        if(is_cancelled(s->cancel)) break;
        if(!DeviceIoControl(s->hVol, FSCTL_ENUM_USN_DATA, &med, sizeof(med), buf, 16*1024*1024, &bytes, NULL)){
            DWORD e = GetLastError(); if(e==ERROR_HANDLE_EOF) break; else { break; }
        }
        DWORD_PTR pRec = (DWORD_PTR)buf + sizeof(USN);
        while(pRec + sizeof(USN_RECORD_V2) <= (DWORD_PTR)buf + bytes){
            USN_RECORD_V2* r = (USN_RECORD_V2*)pRec;
            if(r->RecordLength < sizeof(USN_RECORD_V2)) break;
            wchar_t name[MAX_PATH];
            wcsncpy_s(name, MAX_PATH, (const wchar_t*)((BYTE*)r + r->FileNameOffset), r->FileNameLength/2);
            if(s->streaming_mode || s->map.streaming){
                s->streaming_mode = TRUE;
                frn_emit_direct(s, r);
            } else {
                FrnEntry* fe = frnmap_put(&s->map, r->FileReferenceNumber, r->ParentFileReferenceNumber, name, r->FileAttributes);
                if(fe == FRNMAP_STREAMING_SENTINEL){
                    s->streaming_mode = TRUE;
                    frn_emit_direct(s, r);
                } else if(!fe){
                    VirtualFree(buf,0,MEM_RELEASE);
                    return 1;
                }
            }
            pRec += r->RecordLength;
        }
        med.StartFileReferenceNumber = *(USN*)buf;
    }
    // Second pass: emit work items using adaptive thread pool
    if(s->map.cap && s->map.slots){
        s->next_idx = -1;
        s->pool.work_queue_size = (LONG)s->map.cap;
        pool_init(&s->pool, 1, s->max_threads, map_emit_worker, s);
        WaitForMultipleObjects(s->pool.current_threads, s->pool.threads, TRUE, INFINITE);
        pool_destroy(&s->pool);
    }
    VirtualFree(buf,0,MEM_RELEASE);
    return 0;
}

NTFSScanner* NTFSScanner_Start(const wchar_t* volumeRoot, int threads, MPMCQueue* outQueue, CancelToken* cancelToken){
    USNScanner* s = (USNScanner*)calloc(1,sizeof(USNScanner));
    if(!s) return NULL;
    s->outq = outQueue; s->cancel = cancelToken;
    wcscpy_s(s->volRoot, 8, volumeRoot);
    volume_from_root(volumeRoot, s->volPrefix, 8);
    s->hVol = CreateFileW(s->volPrefix, GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if(s->hVol==INVALID_HANDLE_VALUE){ free(s); return NULL; }
    s->max_threads = threads > 0 ? threads : 1;
    uintptr_t h = _beginthreadex(NULL,0,(unsigned (__stdcall *)(void*))usn_thread,s,0,NULL);
    s->thread = (HANDLE)h;
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
    CancelToken* cancel;
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
        if(is_cancelled(t->cancel)) break;
        if(!DeviceIoControl(t->hVol, FSCTL_READ_USN_JOURNAL, &readData, sizeof(readData), buf, 1024*1024, &bytes, NULL)){
            Sleep(50); continue;
        }
        DWORD_PTR pRec = (DWORD_PTR)buf + sizeof(USN);
        while(pRec + sizeof(USN_RECORD_V2) <= (DWORD_PTR)buf + bytes){
            USN_RECORD_V2* r = (USN_RECORD_V2*)pRec;
            if(r->RecordLength < sizeof(USN_RECORD_V2)) break;
            if((DWORD)r->FileNameOffset + (DWORD)r->FileNameLength > r->RecordLength){
                pRec += r->RecordLength;
                continue;
            }
            // Build parent path best-effort: we don't maintain a full FRN map here; do a stat to reconstruct
            wchar_t name[MAX_PATH];
            wcsncpy_s(name, MAX_PATH, (const wchar_t*)((BYTE*)r + r->FileNameOffset), r->FileNameLength/2);
            if(r->Reason & (USN_REASON_FILE_DELETE | USN_REASON_RENAME_OLD_NAME)){
                FILE_ID_DESCRIPTOR pfid={0}; pfid.dwSize=sizeof(pfid); pfid.Type=FileIdType; pfid.FileId.QuadPart=r->ParentFileReferenceNumber;
                HANDLE hPar = OpenFileById(t->hVol, &pfid, FILE_READ_ATTRIBUTES, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, NULL,0);
                if(hPar!=INVALID_HANDLE_VALUE){
                    wchar_t parent[MAX_LONG_PATH]; DWORD got = GetFinalPathNameByHandleW(hPar, parent, MAX_LONG_PATH, FILE_NAME_NORMALIZED);
                    CloseHandle(hPar);
                    if(got>0 && got<MAX_LONG_PATH){
                        if(wcsncmp(parent, L"\\?\", 4)==0) { memmove(parent, parent+4, (wcslen(parent)-3)*sizeof(wchar_t)); }
                        DbWorkItem* wi = (DbWorkItem*)aligned_malloc(sizeof(DbWorkItem), CACHE_LINE_SIZE);
                        wi->content = NULL;
                        wi->preview = NULL;
                        wcscpy_s(wi->parent_path, MAX_LONG_PATH, parent);
                        wcscpy_s(wi->name, MAX_PATH, name);
                        wi->file_size = wi->creation_time = wi->modified_time = wi->access_time = 0;
                        wi->attributes = 0;
                        wi->clone_id = 0;
                        wi->stage = INDEX_NAMES_ONLY;
                        wi->op = WI_DELETE;
                        while(!MPMC_Push(t->outq, wi)) { SwitchToThread(); }
                    }
                }
            } else {
                HANDLE hFile = INVALID_HANDLE_VALUE;
                FILE_ID_DESCRIPTOR fid = {0}; fid.dwSize=sizeof(fid); fid.Type = FileIdType; fid.FileId.QuadPart = r->FileReferenceNumber;
                hFile = OpenFileById(t->hVol, &fid, FILE_READ_ATTRIBUTES, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, NULL,0);
                if(hFile!=INVALID_HANDLE_VALUE){
                    wchar_t full[MAX_LONG_PATH]; DWORD got = GetFinalPathNameByHandleW(hFile, full, MAX_LONG_PATH, FILE_NAME_NORMALIZED);
                    CloseHandle(hFile);
                    if(got>0 && got<MAX_LONG_PATH){
                        // full is \?\C:\Dir\Name — split into parent/name
                        wchar_t parent[MAX_LONG_PATH]; wcscpy_s(parent, MAX_LONG_PATH, full);
                        if(wcsncmp(parent, L"\\?\", 4)==0) { memmove(parent, parent+4, (wcslen(parent)-3)*sizeof(wchar_t)); }
                        wchar_t* p = wcsrchr(parent, L'\'); if(p){ *p=0; }
                        DbWorkItem* wi = (DbWorkItem*)aligned_malloc(sizeof(DbWorkItem), CACHE_LINE_SIZE);
                        wi->content = NULL;
                        wi->preview = NULL;
                        wcscpy_s(wi->parent_path, MAX_LONG_PATH, parent);
                        wcscpy_s(wi->name, MAX_PATH, name);
                        uint32_t attrs=0; uint64_t sz=0, ct=0, mt=0, at=0;
                        wchar_t fn[MAX_LONG_PATH]; swprintf(fn, MAX_LONG_PATH, L"%s\%s", parent, name);
                        get_file_info_basic(fn, &attrs, &sz, &ct, &mt, &at);
                        wi->attributes = attrs?attrs: r->FileAttributes;
                        wi->file_size = sz; wi->creation_time=ct; wi->modified_time=mt; wi->access_time=at;
                        wi->clone_id = 0;
                        wi->stage = INDEX_METADATA_LIGHT;
                        wi->op = WI_ADD;
                        while(!MPMC_Push(t->outq, wi)) { SwitchToThread(); }
                    }
                }
            }
            pRec += r->RecordLength;
        }
        readData.StartUsn = *(USN*)buf;
    }
    VirtualFree(buf,0,MEM_RELEASE);
    return 0;
}

HANDLE StartUSNTailer(const wchar_t* volumeRoot, MPMCQueue* outQueue, CancelToken* cancelToken){
    wchar_t volprefix[8];
    if(!volume_from_root(volumeRoot, volprefix, 8)) return NULL;
    HANDLE hVol = CreateFileW(volprefix, GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if(hVol==INVALID_HANDLE_VALUE) return NULL;
    TailCtx* t = (TailCtx*)calloc(1,sizeof(TailCtx));
    t->hVol=hVol; t->cancel=cancelToken; t->outq=outQueue;
    wcscpy_s(t->root, 8, volumeRoot);
    uintptr_t th = _beginthreadex(NULL,0,(unsigned (__stdcall *)(void*))tail_thread,t,0,NULL);
    t->thread = (HANDLE)th;
    return t->thread;
}
