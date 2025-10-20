
// ntfs.c — Real NTFS USN journal scan + tail
#define _CRT_SECURE_NO_WARNINGS
#include "core/pch.h"
#include <process.h>

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
    size_t slots_bytes;
    volatile size_t slots_committed;
    NameArena* arena;
    NameArena* current_arena;
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
#define FRN_SMALL_ARENA_CHARS (size_t)(16ULL*1024ULL)
#define FRN_ARENA_POOL_BATCH 32
#define FRN_ARENA_ALIGN_CHARS 2
#define FRN_MMAP_MIN_CHARS (size_t)((64ULL*1024ULL)/sizeof(wchar_t))
#define FRNMAP_COMMIT_GRANULARITY (size_t)(64ULL*1024ULL)

#ifndef _WIN32
#include <sys/mman.h>
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif

#ifdef _WIN32
static size_t frnmap_atomic_read_size(volatile size_t* value){
#if defined(_WIN64)
    return (size_t)InterlockedCompareExchange64((volatile LONG64*)value, 0, 0);
#else
    return (size_t)InterlockedCompareExchange((volatile LONG*)value, 0, 0);
#endif
}

static BOOL frnmap_atomic_compare_exchange_size(volatile size_t* value, size_t expected, size_t desired){
#if defined(_WIN64)
    return InterlockedCompareExchange64((volatile LONG64*)value, (LONG64)desired, (LONG64)expected) == (LONG64)expected;
#else
    return InterlockedCompareExchange((volatile LONG*)value, (LONG)desired, (LONG)expected) == (LONG)expected;
#endif
}
#else
static size_t frnmap_atomic_read_size(volatile size_t* value){
    return *value;
}

static BOOL frnmap_atomic_compare_exchange_size(volatile size_t* value, size_t expected, size_t desired){
    if(*value == expected){
        *value = desired;
        return TRUE;
    }
    return FALSE;
}
#endif

static void frnmap_release_slots(FrnMap* m){
    if(!m || !m->slots){
        return;
    }
#ifdef _WIN32
    VirtualFree(m->slots, 0, MEM_RELEASE);
#else
    munmap(m->slots, m->slots_bytes);
#endif
    m->slots = NULL;
    m->slots_bytes = 0;
    m->slots_committed = 0;
}

static BOOL frnmap_ensure_slot_committed(FrnMap* m, size_t index){
#ifdef _WIN32
    if(!m || !m->slots){
        return FALSE;
    }
    size_t needed_bytes = (index + 1) * sizeof(FrnEntry);
    if(needed_bytes > m->slots_bytes){
        needed_bytes = m->slots_bytes;
    }
    for(;;){
        size_t committed = frnmap_atomic_read_size(&m->slots_committed);
        if(needed_bytes <= committed){
            return TRUE;
        }
        size_t gran = FRNMAP_COMMIT_GRANULARITY;
        size_t target = (needed_bytes + (gran - 1)) & ~(gran - 1);
        if(target > m->slots_bytes){
            target = m->slots_bytes;
        }
        if(target <= committed){
            return TRUE;
        }
        size_t to_commit = target - committed;
        void* addr = (uint8_t*)m->slots + committed;
        if(!VirtualAlloc(addr, to_commit, MEM_COMMIT, PAGE_READWRITE)){
            return FALSE;
        }
        if(frnmap_atomic_compare_exchange_size(&m->slots_committed, committed, committed + to_commit)){
            return TRUE;
        }
    }
#else
    (void)m; (void)index;
#endif
    return TRUE;
}

static BOOL frnmap_allocate_slots(FrnMap* m, size_t slot_count){
    if(!m){
        return FALSE;
    }
    if(slot_count == 0){
        m->slots = NULL;
        m->slots_bytes = 0;
        m->slots_committed = 0;
        return TRUE;
    }
    size_t bytes = slot_count * sizeof(FrnEntry);
#ifdef _WIN32
    FrnEntry* slots = (FrnEntry*)VirtualAlloc(NULL, bytes, MEM_RESERVE, PAGE_READWRITE);
    if(!slots){
        return FALSE;
    }
    size_t initial_commit = bytes < FRNMAP_COMMIT_GRANULARITY ? bytes : FRNMAP_COMMIT_GRANULARITY;
    if(initial_commit){
        if(!VirtualAlloc(slots, initial_commit, MEM_COMMIT, PAGE_READWRITE)){
            VirtualFree(slots, 0, MEM_RELEASE);
            return FALSE;
        }
    }
    m->slots = slots;
    m->slots_bytes = bytes;
    m->slots_committed = initial_commit;
#else
    FrnEntry* slots = (FrnEntry*)mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if(slots == MAP_FAILED){
        m->slots = NULL;
        return FALSE;
    }
    m->slots = slots;
    m->slots_bytes = bytes;
    m->slots_committed = bytes;
#endif
    return TRUE;
}

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
    m->slots = NULL;
    m->slots_bytes = 0;
    m->slots_committed = 0;
    if(!frnmap_allocate_slots(m, m->cap)){
        m->slots = NULL;
        m->slots_bytes = 0;
        m->slots_committed = 0;
    }
    m->count = 0;
    m->arena = NULL;
    m->current_arena = NULL;
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
    frnmap_release_slots(m);
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
    m->slots_bytes = 0;
    m->slots_committed = 0;
    m->cap = 0;
    m->count = 0;
    m->arena = NULL;
    m->current_arena = NULL;
    m->arena_total_bytes = 0;
    m->mmap_total_bytes = 0;
    m->arena_limit_bytes = FRN_ARENA_HEAP_LIMIT_BYTES;
    m->mmap_limit_bytes = FRN_ARENA_MMAP_LIMIT_BYTES;
    m->small_chunk_chars = frn_align_chars(FRN_SMALL_ARENA_CHARS);
    m->arena_free_list = NULL;
    m->arena_pool_blocks = NULL;
    m->streaming = FALSE;
}
static BOOL frnmap_rehash_into(FrnMap* dst, const FrnMap* src){
    for(size_t i=0;i<src->cap;i++){
        if(!src->slots[i].frn) continue;
        FrnEntry e = src->slots[i];
        size_t j = (size_t)(frn_hash(e.frn) & (dst->cap-1));
        if(!frnmap_ensure_slot_committed(dst, j)){
            return FALSE;
        }
        while(dst->slots[j].frn){
            j = (j+1) & (dst->cap-1);
            if(!frnmap_ensure_slot_committed(dst, j)){
                return FALSE;
            }
        }
        dst->slots[j] = e;
    }
    return TRUE;
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
    arena->free_next = NULL;
    *out = arena;
    m->mmap_total_bytes += aligned * sizeof(wchar_t);
    return TRUE;
}

static NameArena* frnmap_push_arena(FrnMap* m, size_t min_chars){
    size_t need = frn_align_chars(min_chars);
    size_t chunk = m->small_chunk_chars;
    if(chunk < need){
        chunk = need;
    }
    size_t chunk_bytes = chunk * sizeof(wchar_t);
    NameArena* arena = NULL;
    if(m->arena_total_bytes + chunk_bytes > m->arena_limit_bytes){
        if(!frnmap_create_mmap_arena(m, chunk, &arena)){
            m->streaming = TRUE;
            return NULL;
        }
    } else {
        arena = frnmap_acquire_node(m);
        if(!arena){
            return NULL;
        }
        arena->base = (wchar_t*)malloc(chunk_bytes);
        if(!arena->base){
            frnmap_release_node(m, arena);
            return NULL;
        }
        arena->used = 0;
        arena->cap = chunk;
        arena->file = NULL;
        arena->mapping = NULL;
        arena->is_mmap = FALSE;
        arena->free_next = NULL;
        m->arena_total_bytes += chunk_bytes;
    }
    arena->next = m->arena;
    arena->free_next = NULL;
    m->arena = arena;
    return arena;
}

static wchar_t* frnmap_alloc_name(FrnMap* m, size_t n){
    if(m->streaming){
        return NULL;
    }
    size_t need = frn_align_chars(n);
    NameArena* arena = m->current_arena;
    if(!arena || arena->used + need > arena->cap){
        arena = frnmap_push_arena(m, need);
        if(!arena){
            return NULL;
        }
        m->current_arena = arena;
    }
    if(arena->used + need > arena->cap){
        return NULL;
    }
    wchar_t* ret = arena->base + arena->used;
    arena->used += need;
    return ret;
}

static BOOL frnmap_resize(FrnMap* m){
    size_t newcap = m->cap ? m->cap*2 : 1024;
    FrnMap tmp = {0};
    tmp.cap = newcap;
    if(!frnmap_allocate_slots(&tmp, newcap)){
        return FALSE;
    }
    tmp.count = m->count;
    if(!frnmap_rehash_into(&tmp, m)){
        frnmap_release_slots(&tmp);
        return FALSE;
    }
    frnmap_release_slots(m);
    m->slots = tmp.slots;
    m->slots_bytes = tmp.slots_bytes;
    m->slots_committed = tmp.slots_committed;
    tmp.slots = NULL;
    tmp.slots_bytes = 0;
    tmp.slots_committed = 0;
    m->cap = newcap;
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
    if(!frnmap_ensure_slot_committed(m, i)){
        return NULL;
    }
    while(m->slots[i].frn && m->slots[i].frn != frn){
        i=(i+1)&(m->cap-1);
        if(!frnmap_ensure_slot_committed(m, i)){
            return NULL;
        }
    }
    BOOL is_new = !m->slots[i].frn;
    size_t name_len = wcslen(name);
    size_t chars_with_null = name_len + 1;
    wchar_t* dst = frnmap_alloc_name(m, chars_with_null);
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
    if(!m || !m->slots || m->cap == 0){
        return NULL;
    }
    size_t i = (size_t)(frn_hash(frn) & (m->cap-1));
    if(!frnmap_ensure_slot_committed(m, i)){
        return NULL;
    }
    while(m->slots[i].frn){
        if(m->slots[i].frn==frn) return &m->slots[i];
        i=(i+1)&(m->cap-1);
        if(!frnmap_ensure_slot_committed(m, i)){
            return NULL;
        }
    }
    return NULL;
}

typedef enum {
    FRN_MODE_BUILDING,
    FRN_MODE_STREAMING,
} FrnMode;

#define USN_STREAM_BATCH 256
#define USN_PARENT_CACHE 256

typedef struct PendingEmit {
    uint64_t frn;
    uint64_t parent_frn;
    wchar_t  name[MAX_PATH];
    uint32_t attrs;
} PendingEmit;

typedef struct ParentPathCacheEntry {
    uint64_t frn;
    wchar_t  path[MAX_LONG_PATH];
    uint64_t last_use;
    BOOL     valid;
} ParentPathCacheEntry;

typedef struct USNScanner {
    HANDLE hVol;
    MPMCQueue* outq;
    CancelToken* cancel;
    wchar_t volRoot[8]; // e.g., L"C:\\"
    wchar_t volPrefix[8]; // e.g., L"\\\\.\\C:" or root path for path building "C:\"
    USN_JOURNAL_DATA_V0 journal_info;
    BOOL journal_info_valid;
    FrnMap map;
    AdaptiveThreadPool pool;
    volatile LONG next_idx;
    int max_threads;
    HANDLE thread;
    HANDLE start_event;
    volatile LONG start_notified;
    volatile LONG start_ok;
    DWORD start_error;
    BOOL streaming_mode;
    BOOL map_emit_async;
    BOOL map_freed;
    PendingEmit pending_batch[USN_STREAM_BATCH];
    size_t pending_count;
    ParentPathCacheEntry parent_cache[USN_PARENT_CACHE];
    size_t parent_cache_count;
    uint64_t parent_cache_clock;
} USNScanner;

static BOOL volume_from_root(const wchar_t* root, wchar_t* volprefix, size_t cch){
    if(!root || wcslen(root)<2 || root[1]!=L':') return FALSE;
    return swprintf(volprefix, cch, L"\\\\.\\%c:", root[0])>0;
}

static void usn_notify_start(USNScanner* s, BOOL ok, DWORD err){
    if(!s || !s->start_event){
        return;
    }
    if(InterlockedCompareExchange(&s->start_notified, 1, 0) == 0){
        s->start_ok = ok ? 1 : 0;
        s->start_error = ok ? ERROR_SUCCESS : err;
        SetEvent(s->start_event);
    }
}

static void usn_log_error(const wchar_t* volume_root, DWORD err, const wchar_t* context){
    if(!context){
        context = L"operation";
    }
    if(!volume_root){
        volume_root = L"<unknown>";
    }
#ifdef _WIN32
    LPWSTR msg = NULL;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD len = FormatMessageW(flags, NULL, err, 0, (LPWSTR)&msg, 0, NULL);
    if(len > 0 && msg){
        while(len > 0 && (msg[len-1] == L'\r' || msg[len-1] == L'\n')){
            msg[--len] = L'\0';
        }
        fwprintf(stderr, L"NTFS scanner %ls for %ls failed (err=%lu: %ls).\n", context, volume_root, (unsigned long)err, msg);
        LocalFree(msg);
        return;
    }
#endif
    fwprintf(stderr, L"NTFS scanner %ls for %ls failed (err=%lu).\n", context, volume_root, (unsigned long)err);
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
        if(wcsncmp(parent_full, L"\\\\?\\", 4)==0){
            memmove(parent_full, parent_full+4, (wcslen(parent_full)-3)*sizeof(wchar_t));
        }
        ok = wcscpy_s(parent, cch, parent_full)==0;
    }
    return ok;
}

static BOOL frn_emit_resolved(USNScanner* s, const wchar_t* parent, const wchar_t* name, uint32_t attrs){
    if(is_cancelled(s->cancel)){
        return FALSE;
    }
    DbWorkItem* wi = acquire_work_item();
    if(!wi){
        return FALSE;
    }
    wi->content = NULL;
    wi->preview = NULL;
    wcscpy_s(wi->parent_path, MAX_LONG_PATH, parent);
    wcscpy_s(wi->name, MAX_PATH, name);
    wi->file_size = 0;
    wi->creation_time = wi->modified_time = wi->access_time = 0;
    wi->attributes = attrs;
    wi->clone_id = 0;
    wi->hash_crc = 0;
    wi->hash_ready = FALSE;
    wi->stage = INDEX_NAMES_ONLY;
    wi->op = WI_ADD;
    while(!MPMC_Push(s->outq, wi)){
        if(is_cancelled(s->cancel)){
            release_work_item(wi);
            return FALSE;
        }
        SwitchToThread();
    }
    return TRUE;
}

static BOOL usn_parent_cache_lookup(USNScanner* s, uint64_t parent_frn, wchar_t* parent, size_t cch){
    if(!s) return FALSE;
    for(size_t i = 0; i < USN_PARENT_CACHE; ++i){
        ParentPathCacheEntry* entry = &s->parent_cache[i];
        if(entry->valid && entry->frn == parent_frn){
            if(wcscpy_s(parent, cch, entry->path) == 0){
                entry->last_use = ++s->parent_cache_clock;
                return TRUE;
            }
            return FALSE;
        }
    }
    return FALSE;
}

static void usn_parent_cache_store(USNScanner* s, uint64_t parent_frn, const wchar_t* parent){
    if(!s || !parent){
        return;
    }
    ParentPathCacheEntry* target = NULL;
    ParentPathCacheEntry* oldest = NULL;
    for(size_t i = 0; i < USN_PARENT_CACHE; ++i){
        ParentPathCacheEntry* entry = &s->parent_cache[i];
        if(entry->valid && entry->frn == parent_frn){
            target = entry;
            break;
        }
        if(!entry->valid){
            if(!target){
                target = entry;
            }
        } else if(!oldest || entry->last_use < oldest->last_use){
            oldest = entry;
        }
    }
    if(!target){
        target = oldest ? oldest : &s->parent_cache[0];
    }
    if(!target->valid && s->parent_cache_count < USN_PARENT_CACHE){
        s->parent_cache_count++;
    }
    target->frn = parent_frn;
    wcscpy_s(target->path, MAX_LONG_PATH, parent);
    target->valid = TRUE;
    target->last_use = ++s->parent_cache_clock;
}

static void emit_direct_batched(USNScanner* s, PendingEmit* batch, size_t count){
    if(!s || !batch || count == 0){
        return;
    }
    for(size_t i = 0; i < count; ++i){
        if(is_cancelled(s->cancel)){
            return;
        }
        PendingEmit* item = &batch[i];
        wchar_t parent[MAX_LONG_PATH];
        BOOL resolved = FALSE;
        if(item->parent_frn && usn_parent_cache_lookup(s, item->parent_frn, parent, MAX_LONG_PATH)){
            resolved = TRUE;
        } else {
            if(!frn_resolve_parent_path(s, item->parent_frn, parent, MAX_LONG_PATH)){
                continue;
            }
            if(item->parent_frn){
                usn_parent_cache_store(s, item->parent_frn, parent);
            }
            resolved = TRUE;
        }
        if(!resolved){
            continue;
        }
        frn_emit_resolved(s, parent, item->name, item->attrs);
    }
}

static void usn_flush_pending_emits(USNScanner* s){
    if(!s) return;
    if(s->pending_count){
        emit_direct_batched(s, s->pending_batch, s->pending_count);
        s->pending_count = 0;
    }
}

static void usn_queue_streaming_emit(USNScanner* s, const USN_RECORD_V2* r){
    if(!s || !r) return;
    if(s->pending_count >= USN_STREAM_BATCH){
        emit_direct_batched(s, s->pending_batch, s->pending_count);
        s->pending_count = 0;
    }
    PendingEmit* slot = &s->pending_batch[s->pending_count++];
    slot->frn = r->FileReferenceNumber;
    slot->parent_frn = r->ParentFileReferenceNumber;
    size_t chars = r->FileNameLength / sizeof(wchar_t);
    wcsncpy_s(slot->name, MAX_PATH, (const wchar_t*)((BYTE*)r + r->FileNameOffset), chars);
    slot->attrs = r->FileAttributes;
}

static void usn_check_async_emit_done(USNScanner* s){
    if(!s || !s->map_emit_async || s->map_freed){
        return;
    }
    if(s->pool.current_threads <= 0){
        pool_destroy(&s->pool);
        frnmap_free(&s->map);
        s->map_emit_async = FALSE;
        s->map_freed = TRUE;
        return;
    }
    DWORD wait = WaitForMultipleObjects(s->pool.current_threads, s->pool.threads, TRUE, 0);
    if(wait == WAIT_OBJECT_0){
        pool_destroy(&s->pool);
        frnmap_free(&s->map);
        s->map_emit_async = FALSE;
        s->map_freed = TRUE;
    }
}

static DWORD WINAPI map_emit_worker(void* p){
    USNScanner* s = (USNScanner*)p;
    for(;;){
        LONG idx = InterlockedIncrement(&s->next_idx) - 1;
        if(idx >= (LONG)s->map.cap) break;
        if(!frnmap_ensure_slot_committed(&s->map, (size_t)idx) || !s->map.slots[idx].frn){
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
        DbWorkItem* wi = acquire_work_item();
        if(!wi){
            InterlockedDecrement(&s->pool.work_queue_size);
            adjust_thread_count(&s->pool);
            if(is_cancelled(s->cancel)) break;
            continue;
        }
        wi->content = NULL; wi->preview = NULL;
        wcscpy_s(wi->parent_path, MAX_LONG_PATH, parent);
        wcscpy_s(wi->name, MAX_PATH, e->name);
        wi->file_size = 0;
        wi->creation_time = wi->modified_time = wi->access_time = 0;
        wi->attributes = e->attrs;
        wi->clone_id = 0;
        wi->hash_crc = 0;
        wi->hash_ready = FALSE;
        wi->stage = INDEX_NAMES_ONLY;
        wi->op = WI_ADD;
        while(!MPMC_Push(s->outq, wi)) { SwitchToThread(); }
        InterlockedDecrement(&s->pool.work_queue_size);
        adjust_thread_count(&s->pool);
        if(is_cancelled(s->cancel)) break;
    }
    return 0;
}

static void usn_emit_buffered_results(USNScanner* s){
    if(!s || s->map_emit_async){
        return;
    }
    if(!s->map.cap || !s->map.slots){
        return;
    }
    s->next_idx = -1;
    s->pool.work_queue_size = (LONG)s->map.cap;
    pool_init(&s->pool, 1, s->max_threads, map_emit_worker, s);
    s->map_emit_async = TRUE;
    s->map_freed = FALSE;
}

static DWORD WINAPI usn_thread(void* p){
    USNScanner* s = (USNScanner*)p;
    FrnMode mode = FRN_MODE_BUILDING;
    size_t last_progress_emit = 0;
    DWORD fatal_error = ERROR_SUCCESS;
    BOOL start_signaled = FALSE;
    BYTE* buf = (BYTE*)VirtualAlloc(NULL, 16*1024*1024, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    if(!buf){
        fatal_error = GetLastError();
        if(fatal_error == ERROR_SUCCESS){
            fatal_error = ERROR_NOT_ENOUGH_MEMORY;
        }
        usn_log_error(s->volRoot, fatal_error, L"failed to allocate USN journal buffer");
        usn_notify_start(s, FALSE, fatal_error);
        return 1;
    }
    // Windows returns ERROR_INVALID_PARAMETER (87) if MinMajorVersion/MaxMajorVersion
    // in MFT_ENUM_DATA_V1 are left unset on some systems. We only need the fields
    // shared with V0, so use the older structure for broader compatibility.
    MFT_ENUM_DATA_V0 med = {0};
    med.StartFileReferenceNumber = 0;
    if(s->journal_info_valid){
        med.LowUsn = s->journal_info.FirstUsn;
        med.HighUsn = s->journal_info.NextUsn;
        wprintf(L"Journal ID: 0x%llx, FirstUsn: 0x%llx, NextUsn: 0x%llx\n",
                (unsigned long long)s->journal_info.UsnJournalID,
                (unsigned long long)s->journal_info.FirstUsn,
                (unsigned long long)s->journal_info.NextUsn);
    } else {
        med.LowUsn = 0;
        med.HighUsn = MAXLONGLONG;
    }
    wprintf(L"Starting NTFS enumeration on volume %ls\n", s->volRoot);
    wprintf(L"Initial MFT start: %llu, USN range: %llu to %llu\n",
            (unsigned long long)med.StartFileReferenceNumber,
            (unsigned long long)med.LowUsn,
            (unsigned long long)med.HighUsn);
    DWORD bytes;
    frnmap_init(&s->map, 1<<20);
    int iteration = 0;
    for(;;){
        if(is_cancelled(s->cancel)) break;
        iteration++;
        if(!DeviceIoControl(s->hVol, FSCTL_ENUM_USN_DATA, &med, sizeof(med), buf, 16*1024*1024, &bytes, NULL)){
            DWORD e = GetLastError();
            if(e == ERROR_HANDLE_EOF){
                wprintf(L"USN enumeration finished (EOF) at iteration %d\n", iteration);
                if(!start_signaled){
                    usn_notify_start(s, TRUE, ERROR_SUCCESS);
                    start_signaled = TRUE;
                }
                break;
            }
            wprintf(L"DeviceIoControl failed on iteration %d with error %lu\n", iteration, (unsigned long)e);
            if(e == ERROR_JOURNAL_NOT_ACTIVE){
                wprintf(L"  \u2192 Journal not active\n");
            } else if(e == ERROR_ACCESS_DENIED){
                wprintf(L"  \u2192 Access denied\n");
            } else {
                wprintf(L"  \u2192 Unknown error\n");
            }
            fatal_error = e ? e : ERROR_GEN_FAILURE;
            if(!start_signaled){
                usn_log_error(s->volRoot, fatal_error, L"failed to enumerate the USN journal");
                usn_notify_start(s, FALSE, fatal_error);
                start_signaled = TRUE;
            } else if(s->start_ok){
                usn_log_error(s->volRoot, fatal_error, L"encountered an error while enumerating the USN journal");
            }
            break;
        }
        if(iteration == 1 || iteration % 100 == 0){
            wprintf(L"Iteration %d: received %lu bytes\n", iteration, (unsigned long)bytes);
        }
        if(!start_signaled){
            usn_notify_start(s, TRUE, ERROR_SUCCESS);
            start_signaled = TRUE;
        }
        DWORD_PTR pRec = (DWORD_PTR)buf + sizeof(USN);
        while(pRec + sizeof(USN_RECORD_V2) <= (DWORD_PTR)buf + bytes){
            USN_RECORD_V2* r = (USN_RECORD_V2*)pRec;
            if(r->RecordLength < sizeof(USN_RECORD_V2)) break;
            if((DWORD)r->FileNameOffset + (DWORD)r->FileNameLength > r->RecordLength){
                pRec += r->RecordLength;
                continue;
            }
            if(mode == FRN_MODE_BUILDING){
                wchar_t name[MAX_PATH];
                size_t name_chars = r->FileNameLength / sizeof(wchar_t);
                wcsncpy_s(name, MAX_PATH, (const wchar_t*)((BYTE*)r + r->FileNameOffset), name_chars);
                FrnEntry* fe = frnmap_put(&s->map, r->FileReferenceNumber, r->ParentFileReferenceNumber, name, r->FileAttributes);
                if(fe == FRNMAP_STREAMING_SENTINEL){
                    mode = FRN_MODE_STREAMING;
                    s->map.streaming = TRUE;
                    s->streaming_mode = TRUE;
                    usn_emit_buffered_results(s);
                    live_updates_push_progress((uint64_t)s->map.count, TRUE);
                    usn_queue_streaming_emit(s, r);
                } else if(!fe){
                    fatal_error = ERROR_OUTOFMEMORY;
                    if(s->start_ok){
                        usn_log_error(s->volRoot, fatal_error, L"ran out of memory while building the NTFS index");
                    }
                    VirtualFree(buf,0,MEM_RELEASE);
                    return 1;
                } else {
                    if(s->map.count - last_progress_emit >= 1000){
                        live_updates_push_progress((uint64_t)s->map.count, FALSE);
                        last_progress_emit = s->map.count;
                    }
                    if(s->map.count > 10000){
                        mode = FRN_MODE_STREAMING;
                        s->map.streaming = TRUE;
                        s->streaming_mode = TRUE;
                        usn_emit_buffered_results(s);
                        live_updates_push_progress((uint64_t)s->map.count, TRUE);
                    }
                }
            } else {
                usn_queue_streaming_emit(s, r);
            }
            pRec += r->RecordLength;
        }
        if(mode == FRN_MODE_STREAMING){
            usn_flush_pending_emits(s);
        }
        med.StartFileReferenceNumber = *(USN*)buf;
        usn_check_async_emit_done(s);
    }
    if(!start_signaled){
        usn_notify_start(s, TRUE, ERROR_SUCCESS);
        start_signaled = TRUE;
    }
    if(mode == FRN_MODE_BUILDING){
        live_updates_push_progress((uint64_t)s->map.count, TRUE);
    }
    usn_flush_pending_emits(s);
    if(s->map_emit_async && !s->map_freed){
        if(s->pool.current_threads > 0){
            WaitForMultipleObjects(s->pool.current_threads, s->pool.threads, TRUE, INFINITE);
        }
        pool_destroy(&s->pool);
        frnmap_free(&s->map);
        s->map_emit_async = FALSE;
        s->map_freed = TRUE;
    } else if(!s->map.streaming && s->map.cap && s->map.slots){
        s->next_idx = -1;
        s->pool.work_queue_size = (LONG)s->map.cap;
        pool_init(&s->pool, 1, s->max_threads, map_emit_worker, s);
        if(s->pool.current_threads > 0){
            WaitForMultipleObjects(s->pool.current_threads, s->pool.threads, TRUE, INFINITE);
        }
        pool_destroy(&s->pool);
        frnmap_free(&s->map);
        s->map_freed = TRUE;
    } else if(!s->map_freed && s->map.cap && s->map.slots){
        frnmap_free(&s->map);
        s->map_freed = TRUE;
    }
    VirtualFree(buf,0,MEM_RELEASE);
    if(fatal_error != ERROR_SUCCESS && s->start_ok){
        return 1;
    }
    return 0;
}

NTFSScanner* NTFSScanner_Start(const wchar_t* volumeRoot, int threads, MPMCQueue* outQueue, CancelToken* cancelToken){
    USNScanner* s = (USNScanner*)calloc(1,sizeof(USNScanner));
    if(!s) return NULL;
    s->outq = outQueue; s->cancel = cancelToken;
    s->journal_info_valid = FALSE;

    size_t root_cch = sizeof(s->volRoot) / sizeof(s->volRoot[0]);
    size_t prefix_cch = sizeof(s->volPrefix) / sizeof(s->volPrefix[0]);
    if(wcscpy_s(s->volRoot, root_cch, volumeRoot) != 0){
        free(s);
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if(!volume_from_root(volumeRoot, s->volPrefix, prefix_cch)){
        free(s);
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }

    DWORD desired_access = GENERIC_READ | GENERIC_WRITE;
    DWORD share_mode = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    DWORD flags = FILE_FLAG_BACKUP_SEMANTICS;
    s->hVol = CreateFileW(s->volPrefix, desired_access, share_mode, NULL, OPEN_EXISTING, flags, NULL);
    if(s->hVol==INVALID_HANDLE_VALUE && GetLastError()==ERROR_ACCESS_DENIED){
        desired_access = GENERIC_READ;
        s->hVol = CreateFileW(s->volPrefix, desired_access, share_mode, NULL, OPEN_EXISTING, flags, NULL);
    }
    if(s->hVol==INVALID_HANDLE_VALUE){
        DWORD err = GetLastError();
        usn_log_error(volumeRoot, err, L"failed to open the NTFS volume");
        free(s);
        SetLastError(err);
        return NULL;
    }

    DWORD bytes = 0;
    USN_JOURNAL_DATA_V0 journal_info = {0};
    if(!DeviceIoControl(s->hVol, FSCTL_QUERY_USN_JOURNAL, NULL, 0, &journal_info, sizeof(journal_info), &bytes, NULL)){
        DWORD err = GetLastError();
        usn_log_error(s->volRoot, err, L"failed to query the USN journal");
        CloseHandle(s->hVol);
        free(s);
        SetLastError(err);
        return NULL;
    }
    s->journal_info = journal_info;
    s->journal_info_valid = TRUE;

    s->max_threads = threads > 0 ? threads : 1;
    s->start_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if(!s->start_event){
        DWORD err = GetLastError();
        CloseHandle(s->hVol);
        free(s);
        SetLastError(err);
        return NULL;
    }
    s->start_notified = 0;
    s->start_ok = 0;
    s->start_error = ERROR_SUCCESS;
    uintptr_t h = _beginthreadex(NULL,0,(unsigned (__stdcall *)(void*))usn_thread,s,0,NULL);
    if(!h){
        DWORD err = GetLastError();
        if(err == ERROR_SUCCESS){
            err = ERROR_NOT_ENOUGH_MEMORY;
        }
        CloseHandle(s->start_event);
        CloseHandle(s->hVol);
        free(s);
        SetLastError(err);
        return NULL;
    }
    s->thread = (HANDLE)h;
    DWORD wait = WaitForSingleObject(s->start_event, INFINITE);
    if(wait != WAIT_OBJECT_0){
        DWORD err = GetLastError();
        if(err == ERROR_SUCCESS){
            err = ERROR_GEN_FAILURE;
        }
        usn_log_error(s->volRoot, err, L"failed while waiting for the NTFS scanner to initialize");
        WaitForSingleObject(s->thread, INFINITE);
        CloseHandle(s->thread);
        CloseHandle(s->start_event);
        CloseHandle(s->hVol);
        free(s);
        SetLastError(err);
        return NULL;
    }
    if(!s->start_ok){
        DWORD err = s->start_error ? s->start_error : ERROR_GEN_FAILURE;
        usn_log_error(s->volRoot, err, L"failed to initialize the NTFS scanner");
        WaitForSingleObject(s->thread, INFINITE);
        CloseHandle(s->thread);
        CloseHandle(s->start_event);
        CloseHandle(s->hVol);
        frnmap_free(&s->map);
        free(s);
        SetLastError(err);
        return NULL;
    }
    return (NTFSScanner*)s;
}
void NTFSScanner_Wait(NTFSScanner* s_){
    USNScanner* s = (USNScanner*)s_;
    if(!s) return;
    if(s->thread){
        WaitForSingleObject(s->thread, INFINITE);
    }
}
void NTFSScanner_Free(NTFSScanner* s_){
    USNScanner* s = (USNScanner*)s_;
    if(!s) return;
    if(s->thread){
        CloseHandle(s->thread);
        s->thread = NULL;
    }
    if(s->start_event){
        CloseHandle(s->start_event);
        s->start_event = NULL;
    }
    if(s->hVol && s->hVol!=INVALID_HANDLE_VALUE){
        CloseHandle(s->hVol);
        s->hVol = INVALID_HANDLE_VALUE;
    }
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
                        if(wcsncmp(parent, L"\\\\?\\", 4)==0) { memmove(parent, parent+4, (wcslen(parent)-3)*sizeof(wchar_t)); }
                        DbWorkItem* wi = acquire_work_item();
                        if(!wi){
                            continue;
                        }
                        wi->content = NULL;
                        wi->preview = NULL;
                        wcscpy_s(wi->parent_path, MAX_LONG_PATH, parent);
                        wcscpy_s(wi->name, MAX_PATH, name);
                        wi->file_size = wi->creation_time = wi->modified_time = wi->access_time = 0;
                        wi->attributes = 0;
                        wi->clone_id = 0;
                        wi->hash_crc = 0;
                        wi->hash_ready = FALSE;
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
                        if(wcsncmp(parent, L"\\\\?\\", 4)==0) { memmove(parent, parent+4, (wcslen(parent)-3)*sizeof(wchar_t)); }
                        wchar_t* p = wcsrchr(parent, L'\\'); if(p){ *p=0; }
                        DbWorkItem* wi = acquire_work_item();
                        if(!wi){
                            continue;
                        }
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
                        wi->hash_crc = 0;
                        wi->hash_ready = FALSE;
                        if(!(wi->attributes & FILE_ATTRIBUTE_DIRECTORY)){
                            BOOL hash_ok = FALSE;
                            uint64_t hash = crc64_file(fn, t->cancel, NULL, NULL, &hash_ok);
                            if(hash_ok){
                                wi->hash_crc = hash;
                                wi->hash_ready = TRUE;
                            }
                        }
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
