
// anything.c — Orchestrator
#define _CRT_SECURE_NO_WARNINGS
#include "core/pch.h"

// Define platform-agnostic constants before any conditional compilation to
// ensure they are always available regardless of the active branch.
#define MAX_INDEXED_CONTENT   ((size_t)1024 * 1024)        // 1MB
#define MAX_TOTAL_EPUB_SIZE   ((size_t)4 * 1024 * 1024)    // 4MB safety cap for aggregated EPUB text
#define MAX_EPUB_HTML_FILES   ((size_t)2048)
#define CP_UTF8 65001

#ifdef _WIN32
#include <intrin.h>
#include <process.h>
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "query.lib")
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <CoreServices/CoreServices.h>
#include <CoreFoundation/CoreFoundation.h>
#endif

#include <ctype.h>
#ifndef _WIN32
#include <strings.h>
#include <stdarg.h>
#include <wctype.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <libpst/libpst.h>
#define Sleep(ms) usleep((ms)*1000)
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define _wcsicmp wcscasecmp
#ifndef INFINITE
#define INFINITE 0xFFFFFFFFu
#endif
static int wcsncasecmp_local(const wchar_t* a, const wchar_t* b, size_t n){
    for(size_t i=0;i<n;i++){
        wchar_t ca = towlower(a[i]);
        wchar_t cb = towlower(b[i]);
        if(ca!=cb || ca==0 || cb==0) return ca - cb;
    }
    return 0;
}
static char* StrStrIA(const char* haystack, const char* needle){
    return strcasestr(haystack, needle);
}
static wchar_t* StrStrIW(const wchar_t* haystack, const wchar_t* needle){
    size_t nlen = wcslen(needle);
    for(const wchar_t* p=haystack; *p; p++){
        if(wcsncasecmp_local(p, needle, nlen) == 0) return (wchar_t*)p;
    }
    return NULL;
}
static int _snwprintf(wchar_t* dst, size_t cch, const wchar_t* fmt, ...){
    va_list ap; va_start(ap, fmt);
    int r = vswprintf(dst, cch, fmt, ap);
    va_end(ap);
    return r;
}
#ifdef _WIN32
static LONG64 atomic_inc64(volatile LONG64* v){ return InterlockedIncrement64(v); }
static LONG64 atomic_dec64(volatile LONG64* v){ return InterlockedDecrement64(v); }
static LONG64 atomic_load64(volatile LONG64* v){ return *v; }
#ifndef FILTER_E_END_OF_CHUNKS
#define FILTER_E_END_OF_CHUNKS ((HRESULT)0x80041780L)
#endif
#ifndef FILTER_E_NO_MORE_TEXT
#define FILTER_E_NO_MORE_TEXT ((HRESULT)0x80041781L)
#endif
#ifndef FILTER_E_NO_TEXT
#define FILTER_E_NO_TEXT ((HRESULT)0x80041782L)
#endif
#else
static LONG64 atomic_inc64(volatile LONG64* v){ return __sync_add_and_fetch(v, 1); }
static LONG64 atomic_dec64(volatile LONG64* v){ return __sync_sub_and_fetch(v, 1); }
static LONG64 atomic_load64(volatile LONG64* v){ return __sync_add_and_fetch(v, 0); }
#endif
#ifdef __APPLE__
static void utf8_to_wide(const char* src, wchar_t* dst, size_t dstlen){
    if(!dstlen) return;
    CFStringRef cf = CFStringCreateWithCString(NULL, src, kCFStringEncodingUTF8);
    if(!cf){ dst[0] = 0; return; }
    CFRange range = {0, CFStringGetLength(cf)};
#if __BIG_ENDIAN__
    CFStringEncoding enc = kCFStringEncodingUTF32BE;
#else
    CFStringEncoding enc = kCFStringEncodingUTF32LE;
#endif
    CFIndex used = 0;
    CFStringGetBytes(cf, range, enc, 0, false, (UInt8*)dst,
                     (dstlen - 1) * sizeof(wchar_t), &used);
    CFRelease(cf);
    size_t chars = (size_t)used / sizeof(wchar_t);
    dst[chars] = 0;
}
static void wide_to_utf8(const wchar_t* src, char* dst, size_t dstlen){
    if(!dstlen) return;
#if __BIG_ENDIAN__
    CFStringEncoding enc = kCFStringEncodingUTF32BE;
#else
    CFStringEncoding enc = kCFStringEncodingUTF32LE;
#endif
    size_t wlen = wcslen(src);
    CFStringRef cf = CFStringCreateWithBytes(NULL, (const UInt8*)src,
                                             wlen * sizeof(wchar_t), enc, false);
    if(!cf){ dst[0] = 0; return; }
    CFStringGetCString(cf, dst, dstlen, kCFStringEncodingUTF8);
    CFRelease(cf);
}
static int MultiByteToWideChar(unsigned int cp, unsigned int flags, const char* src, int srclen, wchar_t* dst, int dstlen){
    (void)cp; (void)flags; (void)srclen;
    if(!dst) {
        CFStringRef cf = CFStringCreateWithCString(NULL, src, kCFStringEncodingUTF8);
        if(!cf) return 0;
        int len = (int)CFStringGetLength(cf) + 1;
        CFRelease(cf);
        return len;
    }
    utf8_to_wide(src, dst, dstlen);
    return (int)wcslen(dst);
}
static int WideCharToMultiByte(unsigned int cp, unsigned int flags, const wchar_t* src, int srclen, char* dst, int dstlen, void* a, void* b){
    (void)cp; (void)flags; (void)srclen; (void)a; (void)b;
    wide_to_utf8(src, dst, dstlen);
    return (int)strlen(dst);
}
#else
static int MultiByteToWideChar(unsigned int cp, unsigned int flags, const char* src, int srclen, wchar_t* dst, int dstlen){
    (void)cp; (void)flags; (void)srclen;
    return mbstowcs(dst, src, dstlen);
}
static int WideCharToMultiByte(unsigned int cp, unsigned int flags, const wchar_t* src, int srclen, char* dst, int dstlen, void* a, void* b){
    (void)cp; (void)flags; (void)srclen; (void)a; (void)b;
    return wcstombs(dst, src, dstlen);
}
#endif
#endif

#define BLOOM_GENERATOR_MIN_THREADS 2
#define BLOOM_GENERATOR_MAX_THREADS 4
#define BLOOM_GENERATOR_BATCH_MIN 128
#define BLOOM_GENERATOR_BATCH_TARGET 1024
#define BLOOM_GENERATOR_BATCH_MAX 4096

typedef struct BloomThreadParam {
    int index;
} BloomThreadParam;

typedef struct WriterSignal {
#ifdef _WIN32
    HANDLE event;
#else
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    BOOL            signaled;
    clockid_t       clock_id;
#endif
} WriterSignal;

typedef struct WriterCtx {
    Db* db;
    int batch_size;
    volatile BOOL done;
    MPMCQueue queue;
    CancelToken cancel;
    ContentThreadPool* content_pool;
    size_t grow_attempts;
    DbWorkItem** backlog;
    size_t backlog_head;
    size_t backlog_tail;
    size_t backlog_count;
    size_t backlog_capacity;
    DWORD push_timeout_ms;
    int    min_batch_size;
    int    max_batch_size;
    DWORD  idle_wait_ms;
    size_t consecutive_full_batches;
    size_t consecutive_idle_waits;
    WriterSignal data_signal;
    int content_threads;
    wchar_t db_path[MAX_PATH];
} WriterCtx;

static const size_t MAP_GROWTH_INCREMENT = 1ull * 1024ull * 1024ull * 1024ull; // 1 GB

static MPMCQueue g_live_updates;
MPMCQueue g_bloom_gen_queue;
MPMCQueue g_bloom_completion_queue;
#ifdef _WIN32
static HANDLE g_bloom_generator_threads[BLOOM_GENERATOR_MAX_THREADS] = {0};
#else
static pthread_t g_bloom_generator_threads[BLOOM_GENERATOR_MAX_THREADS] = {0};
#endif
static BloomThreadParam g_bloom_thread_params[BLOOM_GENERATOR_MAX_THREADS];
static unsigned g_bloom_thread_count = 0;
static volatile BOOL g_bloom_shutdown = FALSE;
static wchar_t g_bloom_db_path[MAX_LONG_PATH];
static BOOL g_live_inited = FALSE;
static WorkItemPool g_work_item_pool = {0};

void live_updates_init(void){
    if(!g_live_inited){
        MPMC_Init(&g_live_updates, 1<<12);
        g_live_inited = TRUE;
    }
}

static void work_item_clear(DbWorkItem* item){
    if(!item) return;
    item->parent_path[0] = L'\0';
    item->name[0] = L'\0';
    item->file_size = 0;
    item->creation_time = 0;
    item->modified_time = 0;
    item->access_time = 0;
    item->attributes = 0;
    item->stage = (IndexingLevel)0;
    item->op = 0;
    item->content = NULL;
    item->preview = NULL;
    item->clone_id = 0;
    item->hash_crc = 0;
    item->hash_ready = FALSE;
}

BOOL work_item_pool_init(size_t capacity){
    if(g_work_item_pool.initialized) return TRUE;
    if(capacity == 0){
        g_work_item_pool.capacity = 0;
        g_work_item_pool.queue_size = 0;
        g_work_item_pool.initialized = FALSE;
        return TRUE;
    }
    g_work_item_pool.items = (DbWorkItem*)aligned_malloc(sizeof(DbWorkItem) * capacity, CACHE_LINE_SIZE);
    if(!g_work_item_pool.items) return FALSE;
    for(size_t i=0;i<capacity;i++){
        work_item_clear(&g_work_item_pool.items[i]);
    }
    LONG queue_size = 1;
    while(queue_size < (LONG)capacity) queue_size <<= 1;
    if(!MPMC_Init(&g_work_item_pool.free_queue, queue_size)){
        aligned_free(g_work_item_pool.items);
        g_work_item_pool.items = NULL;
        return FALSE;
    }
    for(size_t i=0;i<capacity;i++){
        if(!MPMC_Push(&g_work_item_pool.free_queue, &g_work_item_pool.items[i])){
            MPMC_Destroy(&g_work_item_pool.free_queue);
            aligned_free(g_work_item_pool.items);
            g_work_item_pool.items = NULL;
            return FALSE;
        }
    }
    g_work_item_pool.capacity = capacity;
    g_work_item_pool.queue_size = (size_t)queue_size;
    g_work_item_pool.initialized = TRUE;
    return TRUE;
}

void work_item_pool_destroy(void){
    if(!g_work_item_pool.initialized) return;
    void* ptr = NULL;
    DbWorkItem* start = g_work_item_pool.items;
    DbWorkItem* end = start ? start + g_work_item_pool.capacity : start;
    while(MPMC_Pop(&g_work_item_pool.free_queue, &ptr)){
        if(ptr && (ptr < (void*)start || ptr >= (void*)end)){
            DbWorkItem* extra = (DbWorkItem*)ptr;
            if(extra->content){ free(extra->content); extra->content = NULL; }
            if(extra->preview){ free(extra->preview); extra->preview = NULL; }
            aligned_free(extra);
        }
    }
    MPMC_Destroy(&g_work_item_pool.free_queue);
    if(g_work_item_pool.items){
        aligned_free(g_work_item_pool.items);
        g_work_item_pool.items = NULL;
    }
    g_work_item_pool.capacity = 0;
    g_work_item_pool.queue_size = 0;
    g_work_item_pool.initialized = FALSE;
}

DbWorkItem* acquire_work_item(void){
    void* item_ptr = NULL;
    if(g_work_item_pool.initialized && MPMC_Pop(&g_work_item_pool.free_queue, &item_ptr)){
        DbWorkItem* item = (DbWorkItem*)item_ptr;
        work_item_clear(item);
        return item;
    }
    DbWorkItem* fallback = (DbWorkItem*)aligned_malloc(sizeof(DbWorkItem), CACHE_LINE_SIZE);
    if(fallback){
        work_item_clear(fallback);
    }
    return fallback;
}

void release_work_item(DbWorkItem* item){
    if(!item) return;
    if(item->content){ free(item->content); item->content = NULL; }
    if(item->preview){ free(item->preview); item->preview = NULL; }
    work_item_clear(item);
    if(g_work_item_pool.initialized){
        if(MPMC_Push(&g_work_item_pool.free_queue, item)){
            return;
        }
    }
    aligned_free(item);
}

BOOL live_updates_poll(LiveUpdate* out){
    if(!g_live_inited) return FALSE;
    void* p = NULL;
    if(!MPMC_Pop(&g_live_updates, &p)) return FALSE;
    if(!p) return FALSE;
    LiveUpdate* lu = (LiveUpdate*)p;
    *out = *lu;
    _ReadWriteBarrier();
    if(InterlockedDecrement(&lu->refcount) == 0){
        aligned_free(lu);
    }
    return TRUE;
}

static void push_live_update(const DbWorkItem* wi){
    if(!g_live_inited) return;
    LiveUpdate* lu = (LiveUpdate*)aligned_malloc(sizeof(LiveUpdate), CACHE_LINE_SIZE);
    if(!lu) return;
    wcscpy_s(lu->parent_path, MAX_LONG_PATH, wi->parent_path);
    wcscpy_s(lu->name, MAX_PATH, wi->name);
    lu->op = wi->op;
    lu->refcount = 1;
    lu->is_progress = FALSE;
    lu->progress_done = FALSE;
    lu->progress_count = 0;
    _ReadWriteBarrier();
    int tries = 0;
    while(!MPMC_Push(&g_live_updates, lu)){
        if(!g_live_inited || tries++ > 1000){
            aligned_free(lu);
            return;
        }
        Sleep(0);
    }
}

void live_updates_push_progress(uint64_t processed_count, BOOL done){
    if(!g_live_inited) return;
    LiveUpdate* lu = (LiveUpdate*)aligned_malloc(sizeof(LiveUpdate), CACHE_LINE_SIZE);
    if(!lu) return;
    lu->parent_path[0] = L'\0';
    lu->name[0] = L'\0';
    lu->op = WI_ADD;
    lu->refcount = 1;
    lu->is_progress = TRUE;
    lu->progress_done = done;
    lu->progress_count = processed_count;
    _ReadWriteBarrier();
    int tries = 0;
    while(!MPMC_Push(&g_live_updates, lu)){
        if(!g_live_inited || tries++ > 1000){
            aligned_free(lu);
            return;
        }
        Sleep(0);
    }
}

#include "anything/metadata.h"

// ---- MPMC queue implementation ----
typedef enum {
    CONTENT_NONE,
    CONTENT_TEXT,
    CONTENT_IFILTER,
    CONTENT_EMAIL,
    CONTENT_EPUB
#ifndef _WIN32
    , CONTENT_PST
#endif
} ContentMode;

static ContentMode get_content_mode(const wchar_t* name){
    const wchar_t* ext = wcsrchr(name, L'.');
    if(!ext) return CONTENT_NONE;
    ext++;
    if(_wcsicmp(ext,L"txt")==0 || _wcsicmp(ext,L"md")==0 ||
       _wcsicmp(ext,L"c")==0   || _wcsicmp(ext,L"h")==0   ||
       _wcsicmp(ext,L"cpp")==0 || _wcsicmp(ext,L"hpp")==0 ||
       _wcsicmp(ext,L"py")==0  || _wcsicmp(ext,L"js")==0  ||
       _wcsicmp(ext,L"cs")==0  || _wcsicmp(ext,L"vb")==0  ||
       _wcsicmp(ext,L"r")==0   || _wcsicmp(ext,L"java")==0 ||
       _wcsicmp(ext,L"json")==0 || _wcsicmp(ext,L"xml")==0 ||
       _wcsicmp(ext,L"yaml")==0 || _wcsicmp(ext,L"yml")==0 ||
       _wcsicmp(ext,L"toml")==0 || _wcsicmp(ext,L"csv")==0 ||
       _wcsicmp(ext,L"nfo")==0  || _wcsicmp(ext,L"ini")==0 ||
       _wcsicmp(ext,L"log")==0  || _wcsicmp(ext,L"rtf")==0 ||
       _wcsicmp(ext,L"sql")==0)
        return CONTENT_TEXT;

    if(_wcsicmp(ext,L"pdf")==0  || _wcsicmp(ext,L"doc")==0  ||
       _wcsicmp(ext,L"docx")==0 || _wcsicmp(ext,L"ppt")==0  ||
       _wcsicmp(ext,L"pptx")==0 || _wcsicmp(ext,L"xls")==0  ||
       _wcsicmp(ext,L"xlsx")==0)
        return CONTENT_IFILTER;

    if(_wcsicmp(ext,L"eml")==0 || _wcsicmp(ext,L"emlx")==0)
        return CONTENT_EMAIL;

    if(_wcsicmp(ext,L"epub")==0)
        return CONTENT_EPUB;

#ifndef _WIN32
    if(_wcsicmp(ext,L"pst")==0)
        return CONTENT_PST;
#endif

    return CONTENT_NONE;
}

static FILE* open_text_file_stream(const wchar_t* path){
#ifdef _WIN32
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if(h == INVALID_HANDLE_VALUE) return NULL;
    int fd = _open_osfhandle((intptr_t)h, _O_RDONLY | _O_BINARY);
    if(fd == -1){
        CloseHandle(h);
        return NULL;
    }
    FILE* f = _fdopen(fd, "rb");
    if(!f){
        _close(fd);
        return NULL;
    }
    return f;
#else
    char path_mb[MAX_LONG_PATH * 3];
    to_utf8(path, path_mb, sizeof(path_mb));
    path_mb[sizeof(path_mb) - 1] = '\0';
    return fopen(path_mb, "rb");
#endif
}

static char* read_text_file_sequential(const wchar_t* path, size_t* out_len){
    if(out_len) *out_len = 0;
    FILE* f = open_text_file_stream(path);
    if(!f) return NULL;

    size_t capacity = 8192;
    if(capacity > MAX_INDEXED_CONTENT) capacity = MAX_INDEXED_CONTENT;
    char* buffer = (char*)malloc(capacity + 1);
    if(!buffer){
        fclose(f);
        return NULL;
    }

    size_t total = 0;
    while(total < MAX_INDEXED_CONTENT){
        if(capacity - total == 0){
            size_t new_cap = capacity * 2;
            if(new_cap < capacity) new_cap = MAX_INDEXED_CONTENT;
            if(new_cap > MAX_INDEXED_CONTENT) new_cap = MAX_INDEXED_CONTENT;
            if(new_cap == capacity) break;
            char* nb = (char*)realloc(buffer, new_cap + 1);
            if(!nb){
                free(buffer);
                fclose(f);
                return NULL;
            }
            buffer = nb;
            capacity = new_cap;
        }

        size_t space = capacity - total;
        size_t to_read = space;
        size_t remaining_limit = MAX_INDEXED_CONTENT - total;
        if(to_read > remaining_limit) to_read = remaining_limit;

        size_t n = fread(buffer + total, 1, to_read, f);
        if(n == 0){
            if(ferror(f)){
                free(buffer);
                fclose(f);
                return NULL;
            }
            break;
        }
        total += n;
        if(n < to_read) break;
    }

    fclose(f);

    buffer[total] = '\0';
    if(total == 0){
        free(buffer);
        return NULL;
    }

    if(out_len) *out_len = total;
    return buffer;
}

static BOOL needs_thumbnail(const wchar_t* name){
    const wchar_t* ext = wcsrchr(name, L'.');
    if(!ext) return FALSE;
    ext++;
    const wchar_t* exts[] = {L"pdf", L"doc", L"docx", L"ppt", L"pptx", L"xls", L"xlsx"};
    for(size_t i=0;i<sizeof(exts)/sizeof(exts[0]);i++){
        if(_wcsicmp(ext, exts[i])==0) return TRUE;
    }
    return FALSE;
}

static BOOL is_archive_file(const wchar_t* name){
    const wchar_t* ext = wcsrchr(name, L'.');
    if(!ext) return FALSE;
    ext++;
    static const wchar_t* exts[] = {
        L"zip", L"rar", L"7z", L"tar", L"cpio", L"ar", L"iso", L"cab", L"xar", L"lha", L"lzh",
        L"gz", L"bz2", L"xz", L"z", L"lzma", L"lz4", L"zst", L"tgz", L"tbz", L"tbz2", L"txz",
        L"tlz", L"tzst"
    };
    for(size_t i=0;i<sizeof(exts)/sizeof(exts[0]);i++){
        if(_wcsicmp(ext, exts[i])==0) return TRUE;
    }
    return FALSE;
}

static wchar_t* extract_with_filter(const wchar_t* path){
#ifdef _WIN32
    bool do_uninit = false;
    HRESULT hr_init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if(FAILED(hr_init)) return NULL;
    do_uninit = true;

    IFilter* filter = NULL;
    wchar_t* out = NULL;
    size_t cap = 0;
    size_t len = 0;
    bool success = false;

    HRESULT hr = LoadIFilter(path, NULL, NULL, 0, 0, 0, &filter);
    if(FAILED(hr)) goto cleanup;

#ifdef __cplusplus
    hr = filter->Init(IFILTER_INIT_APPLY_INDEX_ATTRIBUTES, 0, NULL, NULL);
#else
    hr = filter->lpVtbl->Init(filter, IFILTER_INIT_APPLY_INDEX_ATTRIBUTES, 0, NULL, NULL);
#endif
    if(FAILED(hr)) goto cleanup;

    cap = 4096;
    out = (wchar_t*)malloc(cap * sizeof(wchar_t));
    if(!out) goto cleanup;

    for(;;){
        WCHAR buf[1024];
        ULONG cch = (ULONG)(sizeof(buf) / sizeof(buf[0]));
#ifdef __cplusplus
        hr = filter->GetText(&cch, buf);
#else
        hr = filter->lpVtbl->GetText(filter, &cch, buf);
#endif
        if(hr == S_FALSE || hr == FILTER_E_END_OF_CHUNKS ||
           hr == FILTER_E_NO_MORE_TEXT || hr == FILTER_E_NO_TEXT || cch == 0){
            break;
        }
        if(FAILED(hr)){
            free(out);
            out = NULL;
            goto cleanup;
        }
        if(len + cch + 1 > cap){
            size_t new_cap = (cap + cch + 1) * 2;
            wchar_t* tmp = (wchar_t*)realloc(out, new_cap * sizeof(wchar_t));
            if(!tmp){
                free(out);
                out = NULL;
                goto cleanup;
            }
            out = tmp;
            cap = new_cap;
        }
        memcpy(out + len, buf, cch * sizeof(wchar_t));
        len += cch;
    }
    if(out){
        out[len] = 0;
        success = true;
    }

cleanup:
    if(!success && out){
        free(out);
        out = NULL;
    }
    if(filter){
#ifdef __cplusplus
        filter->Release();
#else
        filter->lpVtbl->Release(filter);
#endif
    }
    if(do_uninit){
        CoUninitialize();
    }
    return out;
#elif defined(__APPLE__)
    char utf8[MAX_LONG_PATH];
    wcstombs(utf8, path, sizeof(utf8));
    CFStringRef cfpath = CFStringCreateWithCString(NULL, utf8, kCFStringEncodingUTF8);
    if(!cfpath) return NULL;
    CFURLRef url = CFURLCreateWithFileSystemPath(NULL, cfpath, kCFURLPOSIXPathStyle, false);
    CFRelease(cfpath);
    if(!url) return NULL;
    MDItemRef item = MDItemCreate(NULL, url);
    CFRelease(url);
    if(!item) return NULL;
    CFStringRef text = MDItemCopyAttribute(item, kMDItemTextContent);
    CFRelease(item);
    if(!text) return NULL;
    CFIndex len = CFStringGetLength(text);
    CFIndex max = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
    char* buf = (char*)malloc((size_t)max);
    if(!buf){ CFRelease(text); return NULL; }
    if(!CFStringGetCString(text, buf, max, kCFStringEncodingUTF8)){
        free(buf);
        CFRelease(text);
        return NULL;
    }
    CFRelease(text);
    int wlen = MultiByteToWideChar(CP_UTF8, 0, buf, -1, NULL, 0);
    wchar_t* wout = (wchar_t*)malloc(sizeof(wchar_t)*wlen);
    if(wout) MultiByteToWideChar(CP_UTF8, 0, buf, -1, wout, wlen);
    free(buf);
    return wout;
#else
    (void)path;
    return NULL;
#endif
}

static wchar_t* alloc_wide_from_utf8(const char* value){
    if(!value) return NULL;
    int needed = MultiByteToWideChar(CP_UTF8, 0, value, -1, NULL, 0);
    if(needed <= 0) return NULL;
    wchar_t* wtmp = (wchar_t*)malloc((size_t)needed * sizeof(wchar_t));
    if(!wtmp) return NULL;
    if(MultiByteToWideChar(CP_UTF8, 0, value, -1, wtmp, needed) <= 0){
        free(wtmp);
        return NULL;
    }
    return wtmp;
}

static wchar_t* dup_wstring_local(const wchar_t* s){
    if(!s) return NULL;
#ifdef _WIN32
    return _wcsdup(s);
#else
    return wcsdup(s);
#endif
}

static void assign_utf8_if_empty(const char* value, wchar_t** target){
    if(!value || !target) return;
    if(*target) return;
    wchar_t* wide = alloc_wide_from_utf8(value);
    if(!wide) return;
    *target = wide;
}

static void capture_email_header_value(const char* value, wchar_t** out){
    assign_utf8_if_empty(value, out);
}

static void assign_wide_if_empty(const wchar_t* value, wchar_t** target){
    if(!value || !target) return;
    if(*target) return;
    if(!value[0]) return;
    wchar_t* copy = dup_wstring_local(value);
    if(!copy) return;
    *target = copy;
}

static char* copy_email_header_value(const char* line, size_t len, size_t prefix){
    if(len <= prefix) return NULL;
    const char* start = line + prefix;
    size_t value_len = len - prefix;
    while(value_len > 0 && isspace((unsigned char)*start)){
        start++;
        value_len--;
    }
    while(value_len > 0 && isspace((unsigned char)start[value_len - 1])){
        value_len--;
    }
    char* tmp = (char*)malloc(value_len + 1);
    if(!tmp) return NULL;
    memcpy(tmp, start, value_len);
    tmp[value_len] = '\0';
    return tmp;
}

static wchar_t* extract_email_content(const wchar_t* path, wchar_t** author_out, wchar_t** title_out){
    if(author_out) *author_out = NULL;
    if(title_out) *title_out = NULL;
    wchar_t* author_w = NULL;
    wchar_t* title_w = NULL;
    FILE* f = _wfopen(path, L"rb");
    if(!f) return NULL;
    if(fseek(f,0,SEEK_END)!=0){ fclose(f); return NULL; }
    long size = ftell(f);
    if(size < 0){ fclose(f); return NULL; }
    if(size > MAX_INDEXED_CONTENT) size = MAX_INDEXED_CONTENT;
    rewind(f);
    char* buf = (char*)malloc((size_t)size + 1);
    if(!buf){ fclose(f); return NULL; }
    size_t n = fread(buf,1,(size_t)size,f);
    fclose(f);
    buf[n]=0;
    char* header_end = strstr(buf, "\r\n\r\n");
    int advance = 4;
    if(!header_end){ header_end = strstr(buf, "\n\n"); advance = 2; }
    if(header_end){
        char* line = buf;
        while(line < header_end){
            char* next = strstr(line, "\r\n");
            size_t sep = 2;
            if(!next || next > header_end){
                next = strstr(line, "\n");
                sep = 1;
            }
            if(!next || next > header_end) break;
            size_t len = (size_t)(next - line);
            if(_strnicmp(line, "From:",5)==0){
                if(len > 5 && line + 5 <= header_end){
                    char* tmp = copy_email_header_value(line, len, 5);
                    if(tmp){
                        capture_email_header_value(tmp, &author_w);
                        free(tmp);
                    }
                }
            } else if(_strnicmp(line, "Subject:",8)==0){
                if(len > 8 && line + 8 <= header_end){
                    char* tmp = copy_email_header_value(line, len, 8);
                    if(tmp){
                        capture_email_header_value(tmp, &title_w);
                        free(tmp);
                    }
                }
            }
            line = next + sep;
        }
        header_end += advance;
    } else {
        header_end = buf;
    }
    char* body = header_end;
    int wlen = MultiByteToWideChar(CP_UTF8,0,body,-1,NULL,0);
    wchar_t* wbuf = (wchar_t*)malloc(sizeof(wchar_t)*wlen);
    if(wbuf) MultiByteToWideChar(CP_UTF8,0,body,-1,wbuf,wlen);
    free(buf);
    if(author_out){
        *author_out = author_w;
    } else if(author_w){
        free(author_w);
    }
    if(title_out){
        *title_out = title_w;
    } else if(title_w){
        free(title_w);
    }
    return wbuf;
}

static char* strip_html_tags(const char* in){
    size_t len = strlen(in);
    char* out = (char*)malloc(len+1);
    if(!out) return NULL;
    size_t o=0; int tag=0;
    for(size_t i=0;i<len;i++){
        if(in[i]=='<') tag=1;
        else if(in[i]=='>') tag=0;
        else if(!tag) out[o++]=in[i];
    }
    out[o]=0;
    return out;
}

static wchar_t* extract_epub_content(const wchar_t* path, wchar_t** author_out, wchar_t** title_out){
    if(author_out) *author_out = NULL;
    if(title_out) *title_out = NULL;
    wchar_t* author_w = NULL;
    wchar_t* title_w = NULL;
    char u8[MAX_LONG_PATH*3];
    to_utf8(path, u8, sizeof(u8));
    int err=0; zip_t* z = zip_open(u8,0,&err);
    if(!z) return NULL;
    zip_stat_t st; char root[256]={0};
    if(zip_stat(z,"META-INF/container.xml",0,&st)==0){
        char* cbuf=(char*)malloc(st.size+1);
        if(cbuf){
            zip_file_t* cf=zip_fopen(z,"META-INF/container.xml",0);
            zip_fread(cf,cbuf,st.size); zip_fclose(cf); cbuf[st.size]=0;
            char* fp=strstr(cbuf,"full-path=");
            if(fp){ fp=strchr(fp,'"'); if(fp){ fp++; char* end=strchr(fp,'"'); if(end){ size_t l=end-fp; if(l>255) l=255; memcpy(root,fp,l); root[l]=0; }}}
            free(cbuf);
        }
    }
    if(root[0]){
        if(zip_stat(z,root,0,&st)==0){
            char* obuf=(char*)malloc(st.size+1);
            if(obuf){
                zip_file_t* of=zip_fopen(z,root,0);
                zip_fread(of,obuf,st.size); zip_fclose(of); obuf[st.size]=0;
                char* t=strstr(obuf,"<dc:title");
                if(t){ t=strchr(t,'>'); if(t){ t++; char* e=strstr(t,"</dc:title>"); if(e){ char tmp[256]; size_t l=e-t; if(l>255) l=255; memcpy(tmp,t,l); tmp[l]=0; assign_utf8_if_empty(tmp, &title_w); }}}
                char* a=strstr(obuf,"<dc:creator");
                if(a){ a=strchr(a,'>'); if(a){ a++; char* e=strstr(a,"</dc:creator>"); if(e){ char tmp[256]; size_t l=e-a; if(l>255) l=255; memcpy(tmp,a,l); tmp[l]=0; assign_utf8_if_empty(tmp, &author_w); }}}
                free(obuf);
            }
        }
    }
    size_t cap=4096,len=0; char* textbuf=(char*)malloc(cap);
    if(textbuf) textbuf[0]=0;
    size_t html_count = 0;
    zip_int64_t count=zip_get_num_entries(z,0);
    for(zip_int64_t i=0;i<count;i++){
        const char* name=zip_get_name(z,i,0);
        if(!name) continue;
        size_t namelen=strlen(name);
        if(namelen>5 && (_stricmp(name+namelen-5,".html")==0 || _stricmp(name+namelen-6,".xhtml")==0)){
            if(len >= MAX_TOTAL_EPUB_SIZE) break;
            if(html_count >= MAX_EPUB_HTML_FILES) break;
            if(zip_stat_index(z,i,0,&st)!=0) continue;
            if(len + st.size >= MAX_INDEXED_CONTENT) break;
            size_t remaining_space = MAX_TOTAL_EPUB_SIZE > len ? (MAX_TOTAL_EPUB_SIZE - len) : 0;
            if(st.size > remaining_space) st.size = remaining_space;
            char* buf=(char*)malloc(st.size+1);
            if(!buf) continue;
            zip_file_t* f=zip_fopen_index(z,i,0);
            zip_fread(f,buf,st.size); zip_fclose(f); buf[st.size]=0;
            char* stripped=strip_html_tags(buf); free(buf);
            if(!stripped) continue;
            size_t slen=strlen(stripped);
            if(len + slen +1 > MAX_TOTAL_EPUB_SIZE){
                if(len >= MAX_TOTAL_EPUB_SIZE){ free(stripped); break; }
                slen = MAX_TOTAL_EPUB_SIZE - len - 1;
            }
            size_t needed = len + slen + 1;
            if(needed > cap){
                size_t newcap = cap;
                while(newcap < needed){
                    if(newcap >= MAX_TOTAL_EPUB_SIZE) { newcap = MAX_TOTAL_EPUB_SIZE; break; }
                    size_t candidate = newcap * 2;
                    if(candidate <= newcap || candidate > MAX_TOTAL_EPUB_SIZE){
                        newcap = MAX_TOTAL_EPUB_SIZE;
                    } else {
                        newcap = candidate;
                    }
                }
                if(newcap < needed) newcap = needed;
                char* tmp=(char*)realloc(textbuf,newcap);
                if(!tmp){ free(stripped); break; }
                textbuf=tmp; cap=newcap;
            }
            memcpy(textbuf+len,stripped,slen); len+=slen; textbuf[len]=0; free(stripped);
            html_count++;
        }
    }
    zip_close(z);
    wchar_t* wbuf=NULL;
    if(textbuf){
        int wlen=MultiByteToWideChar(CP_UTF8,0,textbuf,-1,NULL,0);
        wbuf=(wchar_t*)malloc(sizeof(wchar_t)*wlen);
        if(wbuf) MultiByteToWideChar(CP_UTF8,0,textbuf,-1,wbuf,wlen);
        free(textbuf);
    }
    if(author_out){
        *author_out = author_w;
    } else if(author_w){
        free(author_w);
    }
    if(title_out){
        *title_out = title_w;
    } else if(title_w){
        free(title_w);
    }
    return wbuf;
}

static void append_utf8_line(char** buf, size_t* len, size_t* cap, const char* src){
    if(!src) return;
    size_t slen = strlen(src);
    if(*len + slen + 2 > MAX_INDEXED_CONTENT){
        if(*len >= MAX_INDEXED_CONTENT) return;
        slen = MAX_INDEXED_CONTENT - *len - 1;
    }
    if(*len + slen + 2 > *cap){
        size_t newcap = (*cap + slen + 2)*2;
        char* tmp = (char*)realloc(*buf, newcap);
        if(!tmp) return;
        *buf = tmp; *cap = newcap;
    }
    memcpy(*buf + *len, src, slen);
    *len += slen;
    (*buf)[(*len)++]='\n';
    (*buf)[*len]=0;
}

#ifndef _WIN32
static void walk_pst_tree(pst_file* pf, pst_desc_tree* node, char** buf, size_t* len, size_t* cap){
    for(pst_desc_tree* cur=node; cur; cur=cur->next){
        pst_item* item = pst_parse_item(pf, cur, NULL);
        if(item){
            pst_convert_utf8_null(item, &item->subject);
            pst_convert_utf8_null(item, &item->body);
            if(item->email){
                pst_convert_utf8_null(item, &item->email->sender_address);
                if(item->email->sender_address.str)
                    append_utf8_line(buf,len,cap,item->email->sender_address.str);
            }
            if(item->subject.str) append_utf8_line(buf,len,cap,item->subject.str);
            if(item->body.str) append_utf8_line(buf,len,cap,item->body.str);
            pst_freeItem(item);
        }
        if(cur->child) walk_pst_tree(pf, cur->child, buf, len, cap);
    }
}

static wchar_t* extract_pst_content(const wchar_t* path, wchar_t** author_out, wchar_t** title_out){
    if(author_out) *author_out = NULL;
    if(title_out) *title_out = NULL;
    char u8[MAX_LONG_PATH*3];
    to_utf8(path, u8, sizeof(u8));
    pst_file pf; memset(&pf,0,sizeof(pf));
    if(pst_open(&pf, u8, NULL)!=0) return NULL;
    if(pst_load_index(&pf)!=0){ pst_close(&pf); return NULL; }
    if(pst_load_extended_attributes(&pf)!=0){ pst_close(&pf); return NULL; }
    pst_item* root = pst_parse_item(&pf, pf.d_head, NULL);
    if(!root){ pst_close(&pf); return NULL; }
    pst_desc_tree* top = pst_getTopOfFolders(&pf, root);
    size_t cap=4096,len=0; char* buf=(char*)malloc(cap);
    if(buf) buf[0]=0;
    if(top && top->child) walk_pst_tree(&pf, top->child, &buf, &len, &cap);
    pst_freeItem(root);
    pst_close(&pf);
    wchar_t* wbuf=NULL;
    if(buf){
        int wlen=MultiByteToWideChar(CP_UTF8,0,buf,-1,NULL,0);
        wbuf=(wchar_t*)malloc(sizeof(wchar_t)*wlen);
        if(wbuf) MultiByteToWideChar(CP_UTF8,0,buf,-1,wbuf,wlen);
        free(buf);
    }
    return wbuf;
}
#endif

static uint16_t exif_rd16(const uint8_t* p, int be){
    return be ? (uint16_t)(p[0]<<8 | p[1]) : (uint16_t)(p[1]<<8 | p[0]);
}

static uint32_t exif_rd32(const uint8_t* p, int be){
    return be ? (uint32_t)(p[0]<<24 | p[1]<<16 | p[2]<<8 | p[3])
               : (uint32_t)(p[3]<<24 | p[2]<<16 | p[1]<<8 | p[0]);
}

static void parse_ifd_strings(const uint8_t* base, size_t len, int be, uint32_t off,
                              wchar_t** camera_out, wchar_t** lens_out){
    if(off >= len) return;
    const uint8_t* p = base + off;
    if(p + 2 > base + len) return;
    uint16_t count = exif_rd16(p, be); p += 2;
    for(uint16_t i = 0; i < count; ++i){
        if(p + 12 > base + len) return;
        uint16_t tag = exif_rd16(p, be);
        uint16_t type = exif_rd16(p + 2, be);
        uint32_t num = exif_rd32(p + 4, be);
        uint32_t valoff = exif_rd32(p + 8, be);
        if(type == 2){
            const uint8_t* val = NULL;
            size_t count_bytes = (size_t)num;
            if(num <= 4){
                val = p + 8;
            } else {
                size_t offset = (size_t)valoff;
                if(offset > len || count_bytes > len - offset){ p += 12; continue; }
                val = base + offset;
            }
            size_t slen = count_bytes < 255 ? count_bytes : 255;
            char tmp[256];
            memcpy(tmp, val, slen);
            tmp[slen] = 0;
            if(tag == 0x0110){
                assign_utf8_if_empty(tmp, camera_out);
            } else if(tag == 0xA434){
                assign_utf8_if_empty(tmp, lens_out);
            }
        } else if(tag == 0x8769){
            parse_ifd_strings(base, len, be, valoff, camera_out, lens_out);
        }
        p += 12;
    }
}

static void extract_exif_metadata_strings(const wchar_t* path, wchar_t** camera_out, wchar_t** lens_out){
    if(camera_out) *camera_out = NULL;
    if(lens_out) *lens_out = NULL;
    char u8[MAX_LONG_PATH];
    to_utf8(path, u8, sizeof(u8));
    FILE* f = fopen(u8, "rb");
    if(!f) return;
    uint8_t buf[64*1024];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    const uint8_t* exif = NULL;
    size_t len = n;
    for(size_t i = 2; i + 4 < n; i++){
        if(buf[i] == 0xFF){
            uint8_t marker = buf[i+1];
            if(marker == 0xE1){
                uint16_t seglen = (buf[i+2]<<8) | buf[i+3];
                if(i + 4 + 6 < n && memcmp(buf + i + 4, "Exif\0\0", 6) == 0){
                    exif = buf + i + 10;
                    len = seglen - 8;
                    break;
                }
                i += 1 + seglen;
            } else {
                uint16_t seglen = (buf[i+2]<<8) | buf[i+3];
                i += 1 + seglen;
            }
        }
    }
    if(!exif) return;
    int be;
    if(exif[0]=='M' && exif[1]=='M') be=1;
    else if(exif[0]=='I' && exif[1]=='I') be=0;
    else return;
    uint32_t ifd0 = exif_rd32(exif+4, be);
    parse_ifd_strings(exif, len, be, ifd0, camera_out, lens_out);
}

static void extract_id3_metadata_strings(const wchar_t* path, wchar_t** artist_out, wchar_t** album_out){
    if(artist_out) *artist_out = NULL;
    if(album_out) *album_out = NULL;
    wchar_t* artist_local = NULL;
    wchar_t* album_local = NULL;
    char u8[MAX_LONG_PATH];
    to_utf8(path, u8, sizeof(u8));
    FILE* f = fopen(u8, "rb");
    if(!f) return;
    uint8_t hdr[10];
    if(fread(hdr,1,10,f)==10 && memcmp(hdr,"ID3",3)==0){
        uint32_t size = ((hdr[6]&0x7F)<<21)|((hdr[7]&0x7F)<<14)|((hdr[8]&0x7F)<<7)|(hdr[9]&0x7F);
        uint8_t* buf=(uint8_t*)malloc(size);
        if(buf){
            size_t got=fread(buf,1,size,f);
            size_t pos=0;
            while(pos+10<=got){
                char id[5]; memcpy(id,buf+pos,4); id[4]=0;
                uint32_t fsize=(buf[pos+4]<<24)|(buf[pos+5]<<16)|(buf[pos+6]<<8)|buf[pos+7];
                if(fsize==0||pos+10+fsize>got) break;
                uint8_t enc=buf[pos+10];
                const uint8_t* data=buf+pos+11; size_t dlen=fsize-1;
                char tmp[256]={0};
                if(enc==0||enc==3){
                    size_t len=dlen<255?dlen:255; memcpy(tmp,data,len); tmp[len]=0;
                } else if(enc==1 && dlen>=2){
                    const uint8_t* p=data;
                    if(p[0]==0xFF && p[1]==0xFE){
                        p+=2; dlen-=2;
                        wchar_t wtmp[256]; size_t wlen=0;
                        while(dlen>=2 && wlen<255){
                            wchar_t ch=p[0]|(p[1]<<8); if(ch==0) break;
                            wtmp[wlen++]=ch; p+=2; dlen-=2;
                        }
                        wtmp[wlen]=0; assign_wide_if_empty(wtmp, strcmp(id,"TPE1")==0?&artist_local:&album_local);
                    } else if(p[0]==0xFE && p[1]==0xFF){
                        p+=2; dlen-=2;
                        wchar_t wtmp[256]; size_t wlen=0;
                        while(dlen>=2 && wlen<255){
                            wchar_t ch=(p[0]<<8)|p[1]; if(ch==0) break;
                            wtmp[wlen++]=ch; p+=2; dlen-=2;
                        }
                        wtmp[wlen]=0; assign_wide_if_empty(wtmp, strcmp(id,"TPE1")==0?&artist_local:&album_local);
                    }
                }
                if(tmp[0]){
                    if(strcmp(id,"TPE1")==0) assign_utf8_if_empty(tmp, &artist_local);
                    else if(strcmp(id,"TALB")==0) assign_utf8_if_empty(tmp, &album_local);
                }
                pos+=10+fsize;
            }
            free(buf);
        }
        fclose(f);
    } else {
        fseek(f,-128,SEEK_END);
        uint8_t v1[128];
        if(fread(v1,1,128,f)==128 && memcmp(v1,"TAG",3)==0){
            char tmp[31]; memcpy(tmp,v1+33,30); tmp[30]=0;
            if(tmp[0]) assign_utf8_if_empty(tmp, &artist_local);
            char tmp2[31]; memcpy(tmp2,v1+63,30); tmp2[30]=0;
            if(tmp2[0]) assign_utf8_if_empty(tmp2, &album_local);
        }
        fclose(f);
    }
    if(artist_out){
        *artist_out = artist_local;
    } else if(artist_local){
        free(artist_local);
    }
    if(album_out){
        *album_out = album_local;
    } else if(album_local){
        free(album_local);
    }
}

static BOOL index_file_content(const wchar_t* parent,
                               const wchar_t* name,
                               wchar_t** content_out,
                               wchar_t** author_out,
                               wchar_t** title_out){
    if(content_out) *content_out = NULL;
    if(author_out) *author_out = NULL;
    if(title_out) *title_out = NULL;
    ContentMode mode = get_content_mode(name);
    if(mode == CONTENT_NONE) return TRUE;
    wchar_t path[MAX_LONG_PATH];
    _snwprintf(path, MAX_LONG_PATH, L"%s\\%s", parent, name);

    wchar_t* author_local = NULL;
    wchar_t* title_local = NULL;
    wchar_t* wbuf = NULL;

    if(mode == CONTENT_TEXT){
        size_t raw_len = 0;
        char* buf = read_text_file_sequential(path, &raw_len);
        if(buf){
            if(!memchr(buf, '\0', raw_len)){
                char* a = StrStrIA(buf, "author:");
                if(a){
                    a += 7;
                    while(*a==' '||*a=='\t') a++;
                    char tmp[256]; size_t len=0;
                    while(a[len] && a[len]!='\r' && a[len]!='\n' && len<255) len++;
                    memcpy(tmp,a,len); tmp[len]=0;
                    assign_utf8_if_empty(tmp, &author_local);
                }
                char* t = StrStrIA(buf, "title:");
                if(t){
                    t += 6;
                    while(*t==' '||*t=='\t') t++;
                    char tmp[256]; size_t len=0;
                    while(t[len] && t[len]!='\r' && t[len]!='\n' && len<255) len++;
                    memcpy(tmp,t,len); tmp[len]=0;
                    assign_utf8_if_empty(tmp, &title_local);
                }
                int wlen = MultiByteToWideChar(CP_UTF8,0,buf,-1,NULL,0);
                wbuf = (wchar_t*)malloc(sizeof(wchar_t)*wlen);
                if(wbuf) MultiByteToWideChar(CP_UTF8,0,buf,-1,wbuf,wlen);
            }
            free(buf);
        }
    } else if(mode == CONTENT_IFILTER){
        wbuf = extract_with_filter(path);
    } else if(mode == CONTENT_EMAIL){
        wbuf = extract_email_content(path, &author_local, &title_local);
    } else if(mode == CONTENT_EPUB){
        wbuf = extract_epub_content(path, &author_local, &title_local);
#ifndef _WIN32
    } else if(mode == CONTENT_PST){
        wbuf = extract_pst_content(path, &author_local, &title_local);
#endif
    }

    if(!wbuf){
        if(author_out){
            *author_out = author_local;
        } else if(author_local){
            free(author_local);
        }
        if(title_out){
            *title_out = title_local;
        } else if(title_local){
            free(title_local);
        }
        return TRUE;
    }

    if(!author_local){
        wchar_t* meta_a = StrStrIW(wbuf, L"author:");
        if(meta_a){
            meta_a += 7;
            while(*meta_a==L' '||*meta_a==L'\t') meta_a++;
            wchar_t tmp[256]; size_t len=0;
            while(meta_a[len] && meta_a[len]!=L'\r' && meta_a[len]!=L'\n' && len<255) len++;
            wcsncpy_s(tmp,256,meta_a,len);
            assign_wide_if_empty(tmp, &author_local);
        }
    }
    if(!title_local){
        wchar_t* meta_t = StrStrIW(wbuf, L"title:");
        if(meta_t){
            meta_t += 6;
            while(*meta_t==L' '||*meta_t==L'\t') meta_t++;
            wchar_t tmp[256]; size_t len=0;
            while(meta_t[len] && meta_t[len]!=L'\r' && meta_t[len]!=L'\n' && len<255) len++;
            wcsncpy_s(tmp,256,meta_t,len);
            assign_wide_if_empty(tmp, &title_local);
        }
    }

    if(content_out){
        *content_out = wbuf;
    } else {
        free(wbuf);
    }

    if(author_out){
        *author_out = author_local;
    } else if(author_local){
        free(author_local);
    }
    if(title_out){
        *title_out = title_local;
    } else if(title_local){
        free(title_local);
    }
    return TRUE;
}

static void content_work_item_cleanup(ContentWorkItem* wi, BOOL free_self){
    if(!wi) return;
    if(wi->initial_content){
        free(wi->initial_content);
        wi->initial_content = NULL;
    }
    if(wi->initial_preview){
        free(wi->initial_preview);
        wi->initial_preview = NULL;
    }
    if(free_self) free(wi);
}

static ContentWorkItem* content_work_item_from_dbwork(DbWorkItem* wi, const DbRecord* base){
    if(!wi || !base) return NULL;
    ContentWorkItem* cwi = (ContentWorkItem*)calloc(1, sizeof(ContentWorkItem));
    if(!cwi) return NULL;
    cwi->rec_id = base->rec_id;
    cwi->base_record = *base;
    wcscpy_s(cwi->parent_path, MAX_LONG_PATH, wi->parent_path);
    wcscpy_s(cwi->name, MAX_PATH, wi->name);
    cwi->initial_content = wi->content;
    wi->content = NULL;
    cwi->initial_preview = wi->preview;
    wi->preview = NULL;
    cwi->clone_id = wi->clone_id;
    cwi->attributes = wi->attributes;
    cwi->precomputed_hash = wi->hash_crc;
    cwi->hash_ready = wi->hash_ready;
    return cwi;
}

static void content_result_free(ContentResultItem* result){
    if(!result) return;
    if(result->content_text){ free(result->content_text); result->content_text = NULL; }
    if(result->preview_text){ free(result->preview_text); result->preview_text = NULL; }
    if(result->author_text){ free(result->author_text); result->author_text = NULL; }
    if(result->title_text){ free(result->title_text); result->title_text = NULL; }
    if(result->camera_text){ free(result->camera_text); result->camera_text = NULL; }
    if(result->lens_text){ free(result->lens_text); result->lens_text = NULL; }
    if(result->artist_text){ free(result->artist_text); result->artist_text = NULL; }
    if(result->album_text){ free(result->album_text); result->album_text = NULL; }
    free(result);
}

static ContentResultItem* content_process_item(ContentWorkItem* wi, CancelToken* cancel){
    if(!wi) return NULL;
    ContentResultItem* result = (ContentResultItem*)calloc(1, sizeof(ContentResultItem));
    if(!result) return NULL;
    result->rec_id = wi->rec_id;
    result->base_record = wi->base_record;
    wcscpy_s(result->parent_path, MAX_LONG_PATH, wi->parent_path);
    wcscpy_s(result->name, MAX_PATH, wi->name);
    result->success = TRUE;

    if(is_cancelled(cancel)){
        result->success = FALSE;
        return result;
    }

    wchar_t full[MAX_LONG_PATH];
    _snwprintf(full, MAX_LONG_PATH, L"%s\\%s", wi->parent_path, wi->name);

    if(wi->initial_content){
        result->content_text = wi->initial_content;
        wi->initial_content = NULL;
    } else {
        wchar_t* content_text = NULL;
        wchar_t* author_text = NULL;
        wchar_t* title_text = NULL;
        if(index_file_content(wi->parent_path, wi->name, &content_text, &author_text, &title_text)){
            result->content_text = content_text;
            result->author_text = author_text;
            result->title_text = title_text;
        } else {
            if(content_text) free(content_text);
            if(author_text) free(author_text);
            if(title_text) free(title_text);
        }
    }

    if(wi->initial_preview){
        result->preview_text = wi->initial_preview;
        wi->initial_preview = NULL;
    } else if(needs_thumbnail(wi->name)){
        result->preview_text = GenerateThumbnail(full);
    }

    wchar_t* camera = NULL;
    wchar_t* lens = NULL;
    extract_exif_metadata_strings(full, &camera, &lens);
    result->camera_text = camera;
    result->lens_text = lens;

    wchar_t* artist = NULL;
    wchar_t* album = NULL;
    extract_id3_metadata_strings(full, &artist, &album);
    result->artist_text = artist;
    result->album_text = album;

    result->needs_archive_index = is_archive_file(wi->name);

    if(wi->hash_ready){
        result->hash_crc = wi->precomputed_hash;
        result->has_hash = TRUE;
    } else if(!is_cancelled(cancel)){
        BOOL hash_ok = FALSE;
        uint64_t hash = crc64_file(full, cancel, NULL, NULL, &hash_ok);
        if(hash_ok){
            result->hash_crc = hash;
            result->has_hash = TRUE;
        } else if(is_cancelled(cancel)){
            result->success = FALSE;
        }
    } else {
        result->success = FALSE;
    }

    return result;
}

typedef struct BatchInternRequest {
    wchar_t*   str;
    uint64_t*  target;
    uint64_t*  normalized_target;
} BatchInternRequest;

static BOOL writer_add_intern_request(BatchInternRequest** reqs, size_t* count, size_t* capacity, wchar_t* str, uint64_t* target, uint64_t* normalized_target){
    if(!str || !target){
        if(str) free(str);
        return TRUE;
    }
    if(*count == *capacity){
        size_t newcap = *capacity ? (*capacity * 2) : 64;
        BatchInternRequest* tmp = (BatchInternRequest*)realloc(*reqs, sizeof(BatchInternRequest) * newcap);
        if(!tmp){
            free(str);
            return FALSE;
        }
        *reqs = tmp;
        *capacity = newcap;
    }
    (*reqs)[*count].str = str;
    (*reqs)[*count].target = target;
    (*reqs)[*count].normalized_target = normalized_target;
    (*count)++;
    return TRUE;
}

static void writer_release_intern_requests(BatchInternRequest* reqs, size_t count){
    if(!reqs) return;
    for(size_t i=0;i<count;i++){
        if(reqs[i].str){
            free(reqs[i].str);
            reqs[i].str = NULL;
        }
    }
}

static BOOL writer_apply_content_result(WriterCtx* ctx,
                                        ContentResultItem* result,
                                        DbRecord** buf,
                                        size_t* buf_capacity,
                                        size_t* in_batch,
                                        BOOL* batch_requires_sync,
                                        BatchInternRequest** intern_requests,
                                        size_t* intern_count,
                                        size_t* intern_capacity){
    if(!ctx || !result) return TRUE;
    if(!result->success) return TRUE;
    DbRecord record = result->base_record;
    if(result->content_text){
        if(!writer_add_intern_request(intern_requests, intern_count, intern_capacity, result->content_text, &record.content_str_id, NULL)){
            return FALSE;
        }
        result->content_text = NULL;
    }
    if(result->author_text){
        if(!writer_add_intern_request(intern_requests, intern_count, intern_capacity, result->author_text, &record.author_str_id, NULL)){
            return FALSE;
        }
        result->author_text = NULL;
    }
    if(result->title_text){
        if(!writer_add_intern_request(intern_requests, intern_count, intern_capacity, result->title_text, &record.title_str_id, NULL)){
            return FALSE;
        }
        result->title_text = NULL;
    }
    if(result->preview_text){
        if(!writer_add_intern_request(intern_requests, intern_count, intern_capacity, result->preview_text, &record.preview_str_id, NULL)){
            return FALSE;
        }
        result->preview_text = NULL;
    }
    if(result->camera_text){
        if(!writer_add_intern_request(intern_requests, intern_count, intern_capacity, result->camera_text, &record.camera_str_id, NULL)){
            return FALSE;
        }
        result->camera_text = NULL;
    }
    if(result->lens_text){
        if(!writer_add_intern_request(intern_requests, intern_count, intern_capacity, result->lens_text, &record.lens_str_id, NULL)){
            return FALSE;
        }
        result->lens_text = NULL;
    }
    if(result->artist_text){
        if(!writer_add_intern_request(intern_requests, intern_count, intern_capacity, result->artist_text, &record.artist_str_id, NULL)){
            return FALSE;
        }
        result->artist_text = NULL;
    }
    if(result->album_text){
        if(!writer_add_intern_request(intern_requests, intern_count, intern_capacity, result->album_text, &record.album_str_id, NULL)){
            return FALSE;
        }
        result->album_text = NULL;
    }
    if(result->has_hash){
        record.hash_crc = result->hash_crc;
    }
    if(!writer_ensure_record_capacity(buf, buf_capacity, *in_batch + 1)){
        return FALSE;
    }
    (*buf)[(*in_batch)++] = record;
    if(batch_requires_sync) *batch_requires_sync = TRUE;
    if(result->needs_archive_index){
        wchar_t full[MAX_LONG_PATH];
        _snwprintf(full, MAX_LONG_PATH, L"%s\\%s", result->parent_path, result->name);
        index_archive(ctx->db, full);
    }
    return TRUE;
}

static int compare_uint64(const void* a, const void* b){
    uint64_t av = *(const uint64_t*)a;
    uint64_t bv = *(const uint64_t*)b;
    if(av < bv) return -1;
    if(av > bv) return 1;
    return 0;
}

#ifdef _WIN32
static unsigned __stdcall BloomGeneratorThread(void* param){
#else
static void* BloomGeneratorThread(void* param){
#endif
    BloomThreadParam* thread_param = (BloomThreadParam*)param;
    (void)thread_param;
    MDB_env* env = NULL;
    if(mdb_env_create(&env) != 0){
#ifdef _WIN32
        return 0;
#else
        return NULL;
#endif
    }
    mdb_env_set_maxdbs(env, 64);
    char utf8[MAX_PATH * 3];
    to_utf8(g_bloom_db_path, utf8, sizeof(utf8));
    if(mdb_env_open(env, utf8, MDB_RDONLY, 0664) != 0){
        mdb_env_close(env);
#ifdef _WIN32
        return 0;
#else
        return NULL;
#endif
    }

    MDB_txn* txn = NULL;
    MDB_dbi dbi_strings = 0;
    if(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn) != 0){
        mdb_env_close(env);
#ifdef _WIN32
        return 0;
#else
        return NULL;
#endif
    }
    if(mdb_dbi_open(txn, "strings", 0, &dbi_strings) != 0){
        mdb_txn_abort(txn);
        mdb_env_close(env);
#ifdef _WIN32
        return 0;
#else
        return NULL;
#endif
    }
    mdb_txn_commit(txn);
    txn = NULL;

    uint64_t batch[BLOOM_GENERATOR_BATCH_MAX];

    while(!g_bloom_shutdown){
        size_t count = 0;
        while(count < BLOOM_GENERATOR_BATCH_MAX && !g_bloom_shutdown){
            void* item_ptr = NULL;
            if(MPMC_Pop(&g_bloom_gen_queue, &item_ptr)){
                uint64_t string_id = (uint64_t)(uintptr_t)item_ptr;
                if(string_id){
                    batch[count++] = string_id;
                }
                if(count >= BLOOM_GENERATOR_BATCH_TARGET){
                    break;
                }
                continue;
            }
            if(count >= BLOOM_GENERATOR_BATCH_MIN){
                break;
            }
            if(count == 0){
                Sleep(2);
            } else {
                Sleep(0);
            }
        }
        if(count == 0){
            continue;
        }

        qsort(batch, count, sizeof(uint64_t), compare_uint64);
        size_t unique = 0;
        for(size_t i=0; i<count; ++i){
            if(i > 0 && batch[i] == batch[i-1]) continue;
            batch[unique++] = batch[i];
        }
        count = unique;
        if(count == 0){
            continue;
        }

        if(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn) != 0){
            Sleep(5);
            continue;
        }

        for(size_t i=0; i<count && !g_bloom_shutdown; ++i){
            uint64_t string_id = batch[i];
            MDB_val key = {.mv_data = &string_id, .mv_size = sizeof(string_id)};
            MDB_val val;
            int rc = mdb_get(txn, dbi_strings, &key, &val);
            if(rc != 0){
                continue;
            }
            MDB_val text_val;
            StringMeta current_meta;
            db_string_value_parse(&val, &text_val, &current_meta, NULL);
            BOOL meta_valid = current_meta.magic0 == STRING_META_MAGIC0 && current_meta.magic1 == STRING_META_MAGIC1;
            if(meta_valid && !current_meta.bloom_pending && current_meta.hash_count > 0 && current_meta.bloom_length > 0){
                continue;
            }
            size_t text_len = text_val.mv_size;
            char* text_copy = (char*)malloc(text_len + 1);
            if(!text_copy){
                continue;
            }
            if(text_len > 0){
                memcpy(text_copy, text_val.mv_data, text_len);
            }
            text_copy[text_len] = '\0';

            uint8_t* bloom_data = NULL;
            size_t bloom_len = 0;
            StringMeta new_meta;
            if(!db_generate_bloom_blob(text_copy, text_len, &new_meta, &bloom_data, &bloom_len)){
                free(text_copy);
                if(bloom_data) free(bloom_data);
                continue;
            }
            free(text_copy);

            BloomResultItem* result = (BloomResultItem*)malloc(sizeof(BloomResultItem));
            if(!result){
                if(bloom_data) free(bloom_data);
                continue;
            }
            result->string_id = string_id;
            result->new_meta = new_meta;
            result->bloom_data = bloom_data;
            result->bloom_data_len = bloom_len;

            BOOL queued = FALSE;
            while(!queued){
                if(MPMC_Push(&g_bloom_completion_queue, result)){
                    queued = TRUE;
                } else if(g_bloom_shutdown){
                    if(result->bloom_data) free(result->bloom_data);
                    free(result);
                    queued = TRUE;
                } else {
                    Sleep(1);
                }
            }
        }

        mdb_txn_abort(txn);
        txn = NULL;
    }

    if(txn) mdb_txn_abort(txn);
    if(env) mdb_env_close(env);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

void bloom_generator_init(const wchar_t* dbPath){
    if(!dbPath) return;
    if(g_bloom_thread_count > 0) return;
    wcscpy_s(g_bloom_db_path, MAX_LONG_PATH, dbPath);
    if(!MPMC_Init(&g_bloom_gen_queue, 1 << 14)) return;
    if(!MPMC_Init(&g_bloom_completion_queue, 1 << 13)){
        MPMC_Destroy(&g_bloom_gen_queue);
        return;
    }
    g_bloom_shutdown = FALSE;
    memset(g_bloom_generator_threads, 0, sizeof(g_bloom_generator_threads));
    memset(g_bloom_thread_params, 0, sizeof(g_bloom_thread_params));
#ifdef _WIN32
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    size_t cpu_count = sysinfo.dwNumberOfProcessors ? (size_t)sysinfo.dwNumberOfProcessors : BLOOM_GENERATOR_MIN_THREADS;
#else
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    size_t cpu_count = (cpus > 0) ? (size_t)cpus : BLOOM_GENERATOR_MIN_THREADS;
#endif
    size_t desired = cpu_count / 2;
    if(desired < BLOOM_GENERATOR_MIN_THREADS) desired = BLOOM_GENERATOR_MIN_THREADS;
    if(desired > BLOOM_GENERATOR_MAX_THREADS) desired = BLOOM_GENERATOR_MAX_THREADS;
    unsigned started = 0;
    for(size_t i=0; i<desired; ++i){
        g_bloom_thread_params[i].index = (int)i;
#ifdef _WIN32
        uintptr_t handle = _beginthreadex(NULL, 0, BloomGeneratorThread, &g_bloom_thread_params[i], 0, NULL);
        if(handle == 0){
            break;
        }
        g_bloom_generator_threads[started++] = (HANDLE)handle;
#else
        if(pthread_create(&g_bloom_generator_threads[started], NULL, BloomGeneratorThread, &g_bloom_thread_params[started]) != 0){
            break;
        }
        started++;
#endif
    }
    g_bloom_thread_count = started;
    if(g_bloom_thread_count == 0){
        g_bloom_shutdown = TRUE;
        MPMC_Destroy(&g_bloom_gen_queue);
        MPMC_Destroy(&g_bloom_completion_queue);
    }
}

void bloom_generator_shutdown(void){
    g_bloom_shutdown = TRUE;
#ifdef _WIN32
    for(unsigned i=0; i<g_bloom_thread_count; ++i){
        if(g_bloom_generator_threads[i]){
            WaitForSingleObject(g_bloom_generator_threads[i], INFINITE);
            CloseHandle(g_bloom_generator_threads[i]);
            g_bloom_generator_threads[i] = NULL;
        }
    }
#else
    for(unsigned i=0; i<g_bloom_thread_count; ++i){
        if(g_bloom_generator_threads[i]){
            pthread_join(g_bloom_generator_threads[i], NULL);
            g_bloom_generator_threads[i] = 0;
        }
    }
#endif
    g_bloom_thread_count = 0;
    if(g_bloom_completion_queue.cells){
        void* item = NULL;
        while(MPMC_Pop(&g_bloom_completion_queue, &item)){
            BloomResultItem* result = (BloomResultItem*)item;
            if(result){
                if(result->bloom_data) free(result->bloom_data);
                free(result);
            }
        }
    }
    if(g_bloom_gen_queue.cells){
        void* item = NULL;
        while(MPMC_Pop(&g_bloom_gen_queue, &item)){
            // discard pending requests
        }
    }
    MPMC_Destroy(&g_bloom_gen_queue);
    MPMC_Destroy(&g_bloom_completion_queue);
}

void bloom_generator_request(uint64_t string_id){
    if(g_bloom_thread_count == 0 || string_id == 0) return;
    while(!MPMC_Push(&g_bloom_gen_queue, (void*)(uintptr_t)string_id)){
        if(g_bloom_shutdown) return;
        Sleep(1);
    }
}

static BOOL writer_drain_bloom_results(WriterCtx* ctx){
    if(!ctx) return TRUE;
    void* completed_ptr = NULL;
    while(MPMC_Pop(&g_bloom_completion_queue, &completed_ptr)){
        BloomResultItem* result = (BloomResultItem*)completed_ptr;
        if(!result) continue;
        if(!db_apply_generated_bloom(ctx->db, result)){
            if(result->bloom_data) free(result->bloom_data);
            free(result);
            return FALSE;
        }
        if(result->bloom_data) free(result->bloom_data);
        free(result);
    }
    return TRUE;
}

static BOOL writer_process_content_results(WriterCtx* ctx,
                                           DbRecord** buf,
                                           size_t* buf_capacity,
                                           size_t* in_batch,
                                           BOOL* batch_requires_sync,
                                           BatchInternRequest** intern_requests,
                                           size_t* intern_count,
                                           size_t* intern_capacity){
    if(!ctx || !ctx->content_pool) return TRUE;
    void* item = NULL;
    while(MPMC_Pop(&ctx->content_pool->result_queue, &item)){
        if(!item) continue;
        ContentResultItem* result = (ContentResultItem*)item;
        BOOL ok = writer_apply_content_result(ctx, result, buf, buf_capacity, in_batch, batch_requires_sync,
                                              intern_requests, intern_count, intern_capacity);
        content_result_free(result);
        if(!ok) return FALSE;
    }
    return TRUE;
}

static BOOL writer_signal_init(WriterSignal* sig){
    if(!sig) return FALSE;
#ifdef _WIN32
    sig->event = CreateEvent(NULL, FALSE, FALSE, NULL);
    return sig->event != NULL;
#else
    if(pthread_mutex_init(&sig->mutex, NULL) != 0) return FALSE;
    pthread_condattr_t attr;
    pthread_condattr_t* attrp = NULL;
    sig->clock_id = CLOCK_REALTIME;
    if(pthread_condattr_init(&attr) == 0){
        attrp = &attr;
#ifdef CLOCK_MONOTONIC
        if(pthread_condattr_setclock(attrp, CLOCK_MONOTONIC) == 0){
            sig->clock_id = CLOCK_MONOTONIC;
        }
#endif
    }
    if(pthread_cond_init(&sig->cond, attrp) != 0){
        if(attrp) pthread_condattr_destroy(attrp);
        pthread_mutex_destroy(&sig->mutex);
        return FALSE;
    }
    if(attrp) pthread_condattr_destroy(attrp);
    sig->signaled = FALSE;
    return TRUE;
#endif
}

static void writer_signal_destroy(WriterSignal* sig){
    if(!sig) return;
#ifdef _WIN32
    if(sig->event){
        CloseHandle(sig->event);
        sig->event = NULL;
    }
#else
    pthread_cond_destroy(&sig->cond);
    pthread_mutex_destroy(&sig->mutex);
    sig->signaled = FALSE;
#endif
}

static void writer_signal_notify(WriterSignal* sig){
    if(!sig) return;
#ifdef _WIN32
    SetEvent(sig->event);
#else
    pthread_mutex_lock(&sig->mutex);
    sig->signaled = TRUE;
    pthread_cond_signal(&sig->cond);
    pthread_mutex_unlock(&sig->mutex);
#endif
}

static BOOL writer_signal_wait(WriterSignal* sig, DWORD timeout_ms, BOOL* timed_out){
    if(!sig) return FALSE;
    if(timed_out) *timed_out = FALSE;
#ifdef _WIN32
    DWORD res = WaitForSingleObject(sig->event, timeout_ms);
    if(res == WAIT_OBJECT_0){
        if(timed_out) *timed_out = FALSE;
        return TRUE;
    }
    if(res == WAIT_TIMEOUT){
        if(timed_out) *timed_out = TRUE;
        return TRUE;
    }
    return FALSE;
#else
    BOOL to = FALSE;
    pthread_mutex_lock(&sig->mutex);
    while(!sig->signaled){
        if(timeout_ms == INFINITE){
            if(pthread_cond_wait(&sig->cond, &sig->mutex) != 0){
                pthread_mutex_unlock(&sig->mutex);
                return FALSE;
            }
        } else {
            struct timespec ts;
            clockid_t cid = sig->clock_id;
            if(clock_gettime(cid, &ts) != 0){
                cid = CLOCK_REALTIME;
                if(clock_gettime(cid, &ts) != 0){
                    pthread_mutex_unlock(&sig->mutex);
                    return FALSE;
                }
            }
            ts.tv_sec += timeout_ms / 1000;
            ts.tv_nsec += (timeout_ms % 1000) * 1000000;
            if(ts.tv_nsec >= 1000000000){
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000;
            }
            int rc = pthread_cond_timedwait(&sig->cond, &sig->mutex, &ts);
            if(rc == ETIMEDOUT){
                to = TRUE;
                break;
            }
            if(rc != 0){
                pthread_mutex_unlock(&sig->mutex);
                return FALSE;
            }
        }
    }
    if(!to && sig->signaled){
        sig->signaled = FALSE;
    }
    pthread_mutex_unlock(&sig->mutex);
    if(timed_out) *timed_out = to;
    return TRUE;
#endif
}

static BOOL writer_ensure_record_capacity(DbRecord** buf, size_t* capacity, size_t required){
    if(!buf || !capacity) return FALSE;
    if(required <= *capacity) return TRUE;
    size_t newcap = *capacity ? *capacity : required;
    while(newcap < required){
        newcap *= 2;
    }
    DbRecord* tmp = (DbRecord*)realloc(*buf, sizeof(DbRecord) * newcap);
    if(!tmp) return FALSE;
    *buf = tmp;
    *capacity = newcap;
    return TRUE;
}

static BOOL writer_resolve_intern_requests(WriterCtx* ctx, BatchInternRequest* reqs, size_t count){
    if(!ctx || !reqs || count==0) return TRUE;
    const wchar_t** strings = (const wchar_t**)malloc(sizeof(wchar_t*) * count);
    uint8_t* needs_norm = (uint8_t*)malloc(sizeof(uint8_t) * count);
    uint64_t* ids = (uint64_t*)malloc(sizeof(uint64_t) * count);
    uint64_t* norm_ids = NULL;
    BOOL any_norm = FALSE;
    if(!strings || !ids || !needs_norm){
        free(strings);
        free(ids);
        free(needs_norm);
        writer_release_intern_requests(reqs, count);
        return FALSE;
    }
    for(size_t i=0;i<count;i++){
        strings[i] = reqs[i].str;
        needs_norm[i] = (reqs[i].normalized_target != NULL) ? 1u : 0u;
        if(needs_norm[i]) any_norm = TRUE;
    }
    if(any_norm){
        norm_ids = (uint64_t*)malloc(sizeof(uint64_t) * count);
        if(!norm_ids){
            free(strings);
            free(ids);
            free(needs_norm);
            writer_release_intern_requests(reqs, count);
            return FALSE;
        }
    }
    db_intern_wstrings_batched(ctx->db, strings, needs_norm, count, ids, norm_ids);
    for(size_t i=0;i<count;i++){
        if(reqs[i].target){
            *(reqs[i].target) = ids[i];
        }
        if(reqs[i].normalized_target && norm_ids){
            *(reqs[i].normalized_target) = norm_ids[i];
        }
        if(reqs[i].str){
            free(reqs[i].str);
            reqs[i].str = NULL;
        }
    }
    free(strings);
    free(ids);
    free(needs_norm);
    if(norm_ids) free(norm_ids);
    return TRUE;
}

static BOOL writer_finalize_batch(WriterCtx* ctx, BatchInternRequest* reqs, size_t* count){
    if(!count) return TRUE;
    size_t n = *count;
    if(n == 0) return TRUE;
    if(!writer_resolve_intern_requests(ctx, reqs, n)){
        *count = 0;
        return FALSE;
    }
    *count = 0;
    return TRUE;
}

// ---- Writer context & thread ----
static void writer_queue_on_push(void* param){
    WriterCtx* ctx = (WriterCtx*)param;
    if(!ctx) return;
    writer_signal_notify(&ctx->data_signal);
}

static BOOL put_batch_with_growth(WriterCtx* ctx, DbRecord* buf, size_t in_batch){
    for(;;){
        if(db_put_records(ctx->db, buf, in_batch)){
            return TRUE;
        }
        const DbError* err = db_last_error(ctx->db);
        if(err->code == DB_ERROR_LMDB && err->detail == MDB_MAP_FULL){
            db_abort_write(ctx->db);
            size_t cur = db_current_mapsize(ctx->db);
            size_t max = db_max_mapsize(ctx->db);
            if(cur >= max){
                fprintf(stderr, "DB mapsize at max; cannot grow beyond %llu bytes\n", (unsigned long long)max);
                return FALSE;
            }
            size_t newsize = cur + MAP_GROWTH_INCREMENT;
            if(newsize > max) newsize = max;
            if(!db_set_mapsize(ctx->db, newsize)){
                const DbError* serr = db_last_error(ctx->db);
                fprintf(stderr, "db_set_mapsize failed: %s (code=%d)\n", serr->message, serr->detail);
                return FALSE;
            }
            if(!db_begin_write(ctx->db)) return FALSE;
            ctx->grow_attempts++;
            continue; // retry put
        } else {
            size_t progress = db_last_write_progress(ctx->db);
            fprintf(stderr, "db_put_records failed after %zu/%zu records: %s (code=%d)\n",
                    progress, in_batch, err->message, err->detail);
            return FALSE;
        }
    }
}

static BOOL writer_backlog_push(WriterCtx* ctx, DbWorkItem* wi){
    if(!ctx) return FALSE;
    if(ctx->backlog_count == ctx->backlog_capacity){
        size_t newcap = ctx->backlog_capacity ? ctx->backlog_capacity * 2 : 64;
        DbWorkItem** items = (DbWorkItem**)malloc(sizeof(DbWorkItem*) * newcap);
        if(!items) return FALSE;
        if(ctx->backlog_count){
            for(size_t i=0;i<ctx->backlog_count;i++){
                size_t idx = (ctx->backlog_head + i) % ctx->backlog_capacity;
                items[i] = ctx->backlog[idx];
            }
            ctx->backlog_head = 0;
            ctx->backlog_tail = ctx->backlog_count;
        } else {
            ctx->backlog_head = 0;
            ctx->backlog_tail = 0;
        }
        free(ctx->backlog);
        ctx->backlog = items;
        ctx->backlog_capacity = newcap;
    }
    ctx->backlog[ctx->backlog_tail] = wi;
    ctx->backlog_tail = (ctx->backlog_tail + 1) % ctx->backlog_capacity;
    ctx->backlog_count++;
    return TRUE;
}

static DbWorkItem* writer_backlog_peek(const WriterCtx* ctx){
    if(!ctx || ctx->backlog_count == 0) return NULL;
    return ctx->backlog[ctx->backlog_head];
}

static void writer_backlog_pop(WriterCtx* ctx){
    if(!ctx || ctx->backlog_count == 0) return;
    ctx->backlog_head = (ctx->backlog_head + 1) % ctx->backlog_capacity;
    ctx->backlog_count--;
}

static void writer_backlog_free(WriterCtx* ctx){
    if(!ctx) return;
    if(ctx->backlog_count){
        for(size_t i=0;i<ctx->backlog_count;i++){
            size_t idx = (ctx->backlog_head + i) % ctx->backlog_capacity;
            DbWorkItem* wi = ctx->backlog[idx];
            if(wi) release_work_item(wi);
        }
    }
    free(ctx->backlog);
    ctx->backlog = NULL;
    ctx->backlog_capacity = 0;
    ctx->backlog_count = 0;
    ctx->backlog_head = 0;
    ctx->backlog_tail = 0;
}

static BOOL push_with_backoff(MPMCQueue* q, void* data, DWORD timeout_ms, const char* owner){
    if(!q) return FALSE;
    ULONGLONG start = GetTickCount64();
    int tries = 0;
    for(;;){
        if(MPMC_Push(q, data)) return TRUE;
        tries++;
        if(tries > 1000){
            fprintf(stderr, "%s: queue saturated, pausing producer to relieve pressure\n", owner ? owner : "queue");
            Sleep(10);
            tries = 0;
        } else {
            SwitchToThread();
        }
        if(timeout_ms != INFINITE){
            ULONGLONG elapsed = GetTickCount64() - start;
            if(elapsed >= timeout_ms) return FALSE;
        }
    }
}

typedef struct ContentWorkerCtx {
    ContentThreadPool* pool;
    Db* ro_db;
} ContentWorkerCtx;

#ifdef _WIN32
static unsigned __stdcall content_worker_thread(void* param)
#else
static void* content_worker_thread(void* param)
#endif
{
    ContentWorkerCtx* wctx = (ContentWorkerCtx*)param;
    ContentThreadPool* pool = wctx ? wctx->pool : NULL;
    if(!pool){
        if(wctx){
            if(wctx->ro_db) db_close(wctx->ro_db);
            free(wctx);
        }
#ifdef _WIN32
        return 0;
#else
        return NULL;
#endif
    }
    for(;;){
        void* item = NULL;
        if(!MPMC_Pop(&pool->work_queue, &item)){
            if(pool->shutting_down){
                Sleep(1);
                continue;
            }
            Sleep(1);
            continue;
        }
        if(item == NULL){
            break;
        }
        ContentWorkItem* wi = (ContentWorkItem*)item;
        ContentResultItem* result = content_process_item(wi, pool->cancel_token);
        if(result){
            if(!push_with_backoff(&pool->result_queue, result, INFINITE, "content result")){
                content_result_free(result);
            }
        }
        content_work_item_cleanup(wi, TRUE);
        atomic_dec64(&pool->pending);
        if(pool->shutting_down && atomic_load64(&pool->pending) == 0){
            // continue draining until sentinel arrives
        }
    }
    if(wctx){
        if(wctx->ro_db) db_close(wctx->ro_db);
        free(wctx);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static ContentThreadPool* content_pool_create(const wchar_t* db_path, CancelToken* cancel, int threads){
    if(!cancel || threads <= 0) return NULL;
    ContentThreadPool* pool = (ContentThreadPool*)calloc(1, sizeof(ContentThreadPool));
    if(!pool) return NULL;
    if(!MPMC_Init(&pool->work_queue, 1 << 12)){
        free(pool);
        return NULL;
    }
    if(!MPMC_Init(&pool->result_queue, 1 << 12)){
        MPMC_Destroy(&pool->work_queue);
        free(pool);
        return NULL;
    }
    pool->cancel_token = cancel;
    pool->thread_count = threads;
    pool->pending = 0;
    pool->shutting_down = FALSE;
    if(db_path){
        wcsncpy_s(pool->db_path, MAX_PATH, db_path, _TRUNCATE);
    } else {
        pool->db_path[0] = 0;
    }
    for(int i = 0; i < threads; ++i){
        ContentWorkerCtx* wctx = (ContentWorkerCtx*)calloc(1, sizeof(ContentWorkerCtx));
        if(!wctx){
            pool->thread_count = i;
            pool->shutting_down = TRUE;
            goto fail;
        }
        wctx->pool = pool;
        wctx->ro_db = NULL;
        if(pool->db_path[0]){
            Db* ro_db = NULL;
            if(db_open_readonly(pool->db_path, &ro_db)){
                wctx->ro_db = ro_db;
            }
        }
        pool->worker_contexts[i] = wctx;
#ifdef _WIN32
        uintptr_t th = _beginthreadex(NULL, 0, content_worker_thread, wctx, 0, NULL);
        if(!th){
            if(wctx->ro_db) db_close(wctx->ro_db);
            free(wctx);
            pool->worker_contexts[i] = NULL;
            pool->thread_count = i;
            pool->shutting_down = TRUE;
            goto fail;
        }
        pool->threads[i] = (HANDLE)th;
#else
        if(pthread_create(&pool->threads[i], NULL, content_worker_thread, wctx) != 0){
            if(wctx->ro_db) db_close(wctx->ro_db);
            free(wctx);
            pool->worker_contexts[i] = NULL;
            pool->thread_count = i;
            pool->shutting_down = TRUE;
            goto fail;
        }
#endif
    }
    return pool;
fail:
    for(int j = 0; j < pool->thread_count; ++j){
        if(pool->worker_contexts[j]){
            // threads already created will clean up their context on exit
        }
    }
    pool->shutting_down = TRUE;
    for(int j = 0; j < pool->thread_count; ++j){
        push_with_backoff(&pool->work_queue, NULL, INFINITE, "content shutdown");
    }
#ifdef _WIN32
    WaitForMultipleObjects(pool->thread_count, pool->threads, TRUE, INFINITE);
    for(int j=0;j<pool->thread_count;j++){
        if(pool->threads[j]) CloseHandle(pool->threads[j]);
    }
#else
    for(int j=0;j<pool->thread_count;j++){
        if(pool->threads[j]) pthread_join(pool->threads[j], NULL);
    }
#endif
    MPMC_Destroy(&pool->work_queue);
    MPMC_Destroy(&pool->result_queue);
    free(pool);
    return NULL;
}

static BOOL content_pool_submit(ContentThreadPool* pool, ContentWorkItem* wi){
    if(!pool || !wi) return FALSE;
    atomic_inc64(&pool->pending);
    if(push_with_backoff(&pool->work_queue, wi, INFINITE, "content work")){
        return TRUE;
    }
    atomic_dec64(&pool->pending);
    return FALSE;
}

static LONG64 content_pool_pending(const ContentThreadPool* pool){
    if(!pool) return 0;
    return atomic_load64(&pool->pending);
}

static void content_pool_shutdown(ContentThreadPool* pool){
    if(!pool) return;
    pool->shutting_down = TRUE;
    for(int i = 0; i < pool->thread_count; ++i){
        push_with_backoff(&pool->work_queue, NULL, INFINITE, "content shutdown");
    }
#ifdef _WIN32
    if(pool->thread_count > 0){
        WaitForMultipleObjects(pool->thread_count, pool->threads, TRUE, INFINITE);
    }
    for(int i = 0; i < pool->thread_count; ++i){
        if(pool->threads[i]){
            CloseHandle(pool->threads[i]);
            pool->threads[i] = NULL;
        }
    }
#else
    for(int i = 0; i < pool->thread_count; ++i){
        if(pool->threads[i]){
            pthread_join(pool->threads[i], NULL);
            pool->threads[i] = 0;
        }
    }
#endif
    MPMC_Destroy(&pool->work_queue);
    MPMC_Destroy(&pool->result_queue);
    free(pool);
}

static BOOL writer_enqueue(WriterCtx* ctx, DbWorkItem* wi, const char* stage){
    if(!ctx || !wi) return FALSE;
    if(push_with_backoff(&ctx->queue, wi, ctx->push_timeout_ms, stage)){
        return TRUE;
    }
    if(writer_backlog_push(ctx, wi)){
        fprintf(stderr, "DbWriterThread: queue full while enqueuing %s work; deferring\n", stage ? stage : "unknown");
        return TRUE;
    }
    fprintf(stderr, "DbWriterThread: failed to enqueue %s work due to memory pressure\n", stage ? stage : "unknown");
    return FALSE;
}

static void writer_drain_backlog(WriterCtx* ctx){
    if(!ctx || ctx->backlog_count == 0) return;
    size_t attempts = ctx->backlog_count;
    while(attempts-- && ctx->backlog_count){
        DbWorkItem* wi = writer_backlog_peek(ctx);
        if(!wi) { writer_backlog_pop(ctx); continue; }
        if(push_with_backoff(&ctx->queue, wi, ctx->push_timeout_ms, "DbWriterThread backlog")){
            writer_backlog_pop(ctx);
        } else {
            break;
        }
    }
}

static DWORD WINAPI DbWriterThread(void* p){
    WriterCtx* ctx = (WriterCtx*)p;
    if(ctx->push_timeout_ms == 0) ctx->push_timeout_ms = 50;
    if(!db_begin_write(ctx->db)) return 1;
    if(ctx->batch_size < ctx->min_batch_size) ctx->batch_size = ctx->min_batch_size;
    if(ctx->batch_size > ctx->max_batch_size) ctx->batch_size = ctx->max_batch_size;

    if(ctx->content_threads > 0 && !ctx->content_pool){
        ctx->content_pool = content_pool_create(ctx->db_path, &ctx->cancel, ctx->content_threads);
        if(!ctx->content_pool){
            fprintf(stderr, "DbWriterThread: failed to start content processing pool, continuing synchronously\n");
        }
    }

    size_t buf_capacity = ctx->batch_size > 0 ? (size_t)ctx->batch_size : 1;
    DbRecord* buf = (DbRecord*)malloc(sizeof(DbRecord) * buf_capacity);
    if(!buf) return 1;

    BatchInternRequest* intern_requests = NULL;
    size_t intern_count = 0;
    size_t intern_capacity = 0;
    size_t in_batch = 0;
    BOOL batch_requires_sync = FALSE;
    BOOL success = TRUE;

    for(;;){
        if(!writer_drain_bloom_results(ctx)){
            success = FALSE;
            goto cleanup;
        }
        writer_drain_backlog(ctx);
        if(!writer_process_content_results(ctx, &buf, &buf_capacity, &in_batch, &batch_requires_sync,
                                           &intern_requests, &intern_count, &intern_capacity)){
            success = FALSE;
            goto cleanup;
        }
        void* item = NULL;
        if(!MPMC_Pop(&ctx->queue, &item)){
            if(!writer_process_content_results(ctx, &buf, &buf_capacity, &in_batch, &batch_requires_sync,
                                               &intern_requests, &intern_count, &intern_capacity)){
                success = FALSE;
                goto cleanup;
            }
            if(ctx->done && ctx->backlog_count == 0){
                LONG64 pending = ctx->content_pool ? content_pool_pending(ctx->content_pool) : 0;
                if(pending == 0) break;
            }
            BOOL timed_out = FALSE;
            if(!writer_signal_wait(&ctx->data_signal, ctx->idle_wait_ms, &timed_out)){
                Sleep(1);
            } else if(timed_out){
                if(ctx->consecutive_idle_waits < SIZE_MAX) ctx->consecutive_idle_waits++;
                ctx->consecutive_full_batches = 0;
                if(ctx->consecutive_idle_waits >= 5 && ctx->batch_size > ctx->min_batch_size){
                    ctx->batch_size = ctx->min_batch_size;
                    ctx->consecutive_idle_waits = 0;
                }
            } else {
                ctx->consecutive_idle_waits = 0;
            }
            continue;
        }
        if(item == NULL){
            LONG64 pending = ctx->content_pool ? content_pool_pending(ctx->content_pool) : 0;
            if(ctx->backlog_count == 0 && pending == 0) break;
            ctx->done = TRUE;
            continue;
        }

        ctx->consecutive_idle_waits = 0;

        DbWorkItem* wi = (DbWorkItem*)item;
        if(wi->stage == INDEX_NAMES_ONLY || wi->op == WI_DELETE) push_live_update(wi);
        if(wi->op == WI_DELETE){
            db_delete_path(ctx->db, wi->parent_path, wi->name);
            batch_requires_sync = TRUE;
            release_work_item(wi);
            continue;
        }

        if(!writer_ensure_record_capacity(&buf, &buf_capacity, in_batch + 1)){
            release_work_item(wi);
            success = FALSE;
            goto cleanup;
        }

        DbRecord r = {0};
        if(wi->stage == INDEX_NAMES_ONLY){
            r.type = (wi->attributes & FILE_ATTRIBUTE_DIRECTORY) ? DB_REC_DIR : DB_REC_FILE;
            r.attributes = wi->attributes;
            wchar_t* parent_copy = dup_wstring_local(wi->parent_path);
            wchar_t* name_copy = dup_wstring_local(wi->name);
            if(!parent_copy || !name_copy){
                if(parent_copy) free(parent_copy);
                if(name_copy) free(name_copy);
                release_work_item(wi);
                success = FALSE;
                goto cleanup;
            }
            if(!writer_add_intern_request(&intern_requests, &intern_count, &intern_capacity, parent_copy, &r.parent_str_id, NULL) ||
               !writer_add_intern_request(&intern_requests, &intern_count, &intern_capacity, name_copy, &r.name_str_id, &r.normalized_name_str_id)){
                release_work_item(wi);
                success = FALSE;
                goto cleanup;
            }
            buf[in_batch++] = r;
            DbWorkItem* next = acquire_work_item();
            if(next){
                wcscpy_s(next->parent_path, MAX_LONG_PATH, wi->parent_path);
                wcscpy_s(next->name, MAX_PATH, wi->name);
                next->file_size = next->creation_time = next->modified_time = next->access_time = 0;
                next->attributes = wi->attributes;
                next->clone_id = 0;
                next->hash_crc = wi->hash_crc;
                next->hash_ready = wi->hash_ready;
                next->stage = INDEX_METADATA_LIGHT; next->op = WI_ADD;
                if(!writer_enqueue(ctx, next, "metadata-light")){
                    release_work_item(next);
                }
            }
            release_work_item(wi);
        } else if(wi->stage == INDEX_METADATA_LIGHT){
            batch_requires_sync = TRUE;
            r.type = (wi->attributes & FILE_ATTRIBUTE_DIRECTORY) ? DB_REC_DIR : DB_REC_FILE;
            r.attributes = wi->attributes;
            wchar_t* parent_copy = dup_wstring_local(wi->parent_path);
            wchar_t* name_copy = dup_wstring_local(wi->name);
            if(!parent_copy || !name_copy){
                if(parent_copy) free(parent_copy);
                if(name_copy) free(name_copy);
                release_work_item(wi);
                success = FALSE;
                goto cleanup;
            }
            if(!writer_add_intern_request(&intern_requests, &intern_count, &intern_capacity, parent_copy, &r.parent_str_id, NULL) ||
               !writer_add_intern_request(&intern_requests, &intern_count, &intern_capacity, name_copy, &r.name_str_id, &r.normalized_name_str_id)){
                release_work_item(wi);
                success = FALSE;
                goto cleanup;
            }
            wchar_t full[MAX_LONG_PATH];
            _snwprintf(full, MAX_LONG_PATH, L"%s\\%s", wi->parent_path, wi->name);
            uint32_t attrs=0; uint64_t sz=0, ct=0, mt=0, at=0;
            get_file_info_basic(full, &attrs, &sz, &ct, &mt, &at);
            r.attributes = attrs;
            r.file_size = sz;
            r.creation_time = ct;
            r.modified_time = mt;
            r.access_time   = at;
            buf[in_batch++] = r;
            if(!(attrs & FILE_ATTRIBUTE_DIRECTORY)){
                DbWorkItem* next = acquire_work_item();
                if(next){
                    wcscpy_s(next->parent_path, MAX_LONG_PATH, wi->parent_path);
                    wcscpy_s(next->name, MAX_PATH, wi->name);
                    next->file_size = sz; next->creation_time=ct; next->modified_time=mt; next->access_time=at;
                    next->attributes = attrs;
                    next->clone_id = 0;
                    next->hash_crc = wi->hash_crc;
                    next->hash_ready = wi->hash_ready;
                    next->stage = INDEX_FULL_CONTENT; next->op = WI_ADD;
                    if(!writer_enqueue(ctx, next, "full-content")){
                        release_work_item(next);
                    }
                }
            }
            release_work_item(wi);
        } else {
            batch_requires_sync = TRUE;
            DbRecord existing;
            if(db_get_record_by_path(ctx->db, wi->parent_path, wi->name, &existing)){
                if(existing.type == DB_REC_FILE){
                    BOOL offloaded = FALSE;
                    if(ctx->content_pool){
                        ContentWorkItem* cwi = content_work_item_from_dbwork(wi, &existing);
                        if(cwi){
                            if(content_pool_submit(ctx->content_pool, cwi)){
                                offloaded = TRUE;
                            } else {
                                wi->content = cwi->initial_content;
                                wi->preview = cwi->initial_preview;
                                cwi->initial_content = NULL;
                                cwi->initial_preview = NULL;
                                content_work_item_cleanup(cwi, TRUE);
                            }
                        }
                    }
                    if(offloaded){
                        release_work_item(wi);
                        continue;
                    }
                    ContentWorkItem stack_item;
                    memset(&stack_item, 0, sizeof(stack_item));
                    stack_item.rec_id = existing.rec_id;
                    stack_item.base_record = existing;
                    wcscpy_s(stack_item.parent_path, MAX_LONG_PATH, wi->parent_path);
                    wcscpy_s(stack_item.name, MAX_PATH, wi->name);
                    stack_item.initial_content = wi->content;
                    stack_item.initial_preview = wi->preview;
                    stack_item.clone_id = wi->clone_id;
                    stack_item.attributes = wi->attributes;
                    stack_item.precomputed_hash = wi->hash_crc;
                    stack_item.hash_ready = wi->hash_ready;
                    wi->content = NULL;
                    wi->preview = NULL;
                    ContentResultItem* immediate = content_process_item(&stack_item, &ctx->cancel);
                    content_work_item_cleanup(&stack_item, FALSE);
                    if(immediate){
                        BOOL applied = writer_apply_content_result(ctx, immediate, &buf, &buf_capacity, &in_batch,
                                                                   &batch_requires_sync, &intern_requests, &intern_count, &intern_capacity);
                        content_result_free(immediate);
                        if(!applied){
                            release_work_item(wi);
                            success = FALSE;
                            goto cleanup;
                        }
                    }
                }
            }
            release_work_item(wi);
        }

        if(in_batch >= (size_t)ctx->batch_size){
            if(!writer_finalize_batch(ctx, intern_requests, &intern_count)){
                success = FALSE;
                goto cleanup;
            }
            if(!put_batch_with_growth(ctx, buf, in_batch)) { success = FALSE; goto cleanup; }
            if(!db_commit_write_ex(ctx->db, batch_requires_sync)) { success = FALSE; goto cleanup; }
            if(!db_begin_write(ctx->db))  { success = FALSE; goto cleanup; }
            in_batch = 0;
            batch_requires_sync = FALSE;
            if(ctx->consecutive_full_batches < SIZE_MAX) ctx->consecutive_full_batches++;
            ctx->consecutive_idle_waits = 0;
            if(ctx->consecutive_full_batches >= 3 && ctx->batch_size < ctx->max_batch_size){
                ctx->batch_size = ctx->max_batch_size;
                ctx->consecutive_full_batches = 0;
            }
            if(!writer_ensure_record_capacity(&buf, &buf_capacity, ctx->batch_size)){
                success = FALSE;
                goto cleanup;
            }
        }
    }

    if(ctx->content_pool){
        while(content_pool_pending(ctx->content_pool) > 0){
            if(!writer_process_content_results(ctx, &buf, &buf_capacity, &in_batch, &batch_requires_sync,
                                               &intern_requests, &intern_count, &intern_capacity)){
                success = FALSE;
                goto cleanup;
            }
            if(!writer_drain_bloom_results(ctx)){
                success = FALSE;
                goto cleanup;
            }
            Sleep(1);
        }
        if(!writer_process_content_results(ctx, &buf, &buf_capacity, &in_batch, &batch_requires_sync,
                                           &intern_requests, &intern_count, &intern_capacity)){
            success = FALSE;
            goto cleanup;
        }
        if(!writer_drain_bloom_results(ctx)){
            success = FALSE;
            goto cleanup;
        }
    }

    if(in_batch){
        if(!writer_finalize_batch(ctx, intern_requests, &intern_count)){
            success = FALSE;
            goto cleanup;
        }
        if(!put_batch_with_growth(ctx, buf, in_batch)) { success = FALSE; goto cleanup; }
        if(!db_commit_write_ex(ctx->db, batch_requires_sync)) { success = FALSE; goto cleanup; }
        ctx->consecutive_full_batches = 0;
    } else {
        writer_release_intern_requests(intern_requests, intern_count);
        intern_count = 0;
        db_commit_write_ex(ctx->db, batch_requires_sync);
    }

    if(!writer_drain_bloom_results(ctx)){
        success = FALSE;
        goto cleanup;
    }

cleanup:
    writer_release_intern_requests(intern_requests, intern_count);
    free(intern_requests);
    free(buf);
    if(ctx->content_pool){
        content_pool_shutdown(ctx->content_pool);
        ctx->content_pool = NULL;
    }
    writer_backlog_free(ctx);
    return success ? 0 : 1;
}

// ---- CLI parsing ----
typedef struct {
    wchar_t dbPath[MAX_PATH];
    wchar_t rootPath[MAX_LONG_PATH];
    int threads;
    int batch;
    BOOL use_ntfs;
    BOOL tail_changes;
    BOOL all_drives;
} Args;

static void usage(void){
    wprintf(L"anything.exe index --db <path> (--root <folder> | --all-drives) [--threads N] [--batch N] [--ntfs] [--tail]\n");
    wprintf(L"anything.exe compress --db <path> --out <dest>\n");
}

static BOOL parse_args(int argc, wchar_t** argv, Args* a){
    ZeroMemory(a, sizeof(*a));
    a->threads = g_config.default_index_threads;
    a->batch = g_config.default_batch;
    a->use_ntfs=FALSE; a->tail_changes=FALSE; a->all_drives=FALSE;
    for(int i=1;i<argc;i++){
        if(wcscmp(argv[i], L"index")==0){ continue; }
        else if(wcscmp(argv[i], L"--db")==0 && i+1<argc){ wcscpy_s(a->dbPath, MAX_PATH, argv[++i]); }
        else if(wcscmp(argv[i], L"--root")==0 && i+1<argc){ wcscpy_s(a->rootPath, MAX_LONG_PATH, argv[++i]); }
        else if(wcscmp(argv[i], L"--threads")==0 && i+1<argc){ a->threads = _wtoi(argv[++i]); }
        else if(wcscmp(argv[i], L"--batch")==0 && i+1<argc){ a->batch = _wtoi(argv[++i]); }
        else if(wcscmp(argv[i], L"--ntfs")==0){ a->use_ntfs = TRUE; }
        else if(wcscmp(argv[i], L"--tail")==0){ a->tail_changes = TRUE; }
        else if(wcscmp(argv[i], L"--all-drives")==0){ a->all_drives = TRUE; }
        else { usage(); return FALSE; }
    }
    if(!a->dbPath[0] || (!a->rootPath[0] && !a->all_drives)){ usage(); return FALSE; }
    if(a->threads<1) a->threads=1;
    if(a->threads>g_config.max_index_threads) a->threads=g_config.max_index_threads;
    if(a->batch<1000) a->batch=1000;
    return TRUE;
}

static DWORD WINAPI scan_drive_thread(void* p){
    struct { wchar_t root[8]; WriterCtx* ctx; BOOL use_ntfs; int threads; } *in = p;
    (void)in->use_ntfs; // selection now handled internally
    FileScanner* fs = FileScanner_Start(in->root, in->threads, &in->ctx->queue, &in->ctx->cancel);
    if(fs){
        FileScanner_Wait(fs);
        FileScanner_Free(fs);
    }
    free(in);
    return 0;
}

int wmain(int argc, wchar_t** argv){
    config_init_default();
    config_load_file(L"anything.conf");
    enterprise_deploy_msi();
    if(argc>1 && wcscmp(argv[1], L"compress")==0){
        const wchar_t* dbPath=NULL; const wchar_t* outPath=NULL;
        for(int i=2;i<argc;i++){
            if(wcscmp(argv[i],L"--db")==0 && i+1<argc){ dbPath=argv[++i]; }
            else if(wcscmp(argv[i],L"--out")==0 && i+1<argc){ outPath=argv[++i]; }
            else { usage(); return 1; }
        }
        if(!dbPath || !outPath){ usage(); return 1; }
        Db* cdb=NULL;
        if(!db_open_readonly(dbPath, &cdb)){
            fwprintf(stderr, L"Failed to open DB at %s\n", dbPath);
            return 1;
        }
        BOOL ok = db_compress(cdb, outPath);
        if(!ok){
            const DbError* derr = db_last_error(cdb);
            fwprintf(stderr, L"Compression failed (err=%d: %hs)\n", derr->detail, derr->message);
        }
        db_close(cdb);
        return ok?0:1;
    }

    Args args;
    live_updates_init();
    if(!parse_args(argc, argv, &args)) return 1;
    enterprise_ad_authenticate("user", "");
    enterprise_index_network("\\\\networkshare");

    Db* db=NULL;
    if(!db_create(args.dbPath, /*init_mb*/1024, /*max_mb*/16384, &db)){
        const DbError* cerr = db ? db_last_error(db) : NULL;
        fwprintf(stderr, L"Failed to create/open DB at %s (err=%d: %hs)\n", args.dbPath, cerr?cerr->detail:0, cerr?cerr->message:"");
        return 1;
    }
    if(!db_set_bulk_mode(db, TRUE, 5)){
        const DbError* derr = db_last_error(db);
        fwprintf(stderr, L"Warning: failed to enable bulk DB mode (err=%d: %hs)\n", derr?derr->detail:0, derr?derr->message:"");
    }

    size_t pool_bytes = dynamic_work_mem();
    size_t pool_capacity = pool_bytes / sizeof(DbWorkItem);
    if(pool_capacity == 0) pool_capacity = 1;
    if(!work_item_pool_init(pool_capacity)){
        fwprintf(stderr, L"Warning: failed to initialize DbWorkItem pool (%zu items)\n", pool_capacity);
    }

    WriterCtx ctx = {0};
    ctx.db = db; ctx.batch_size = args.batch; ctx.done=FALSE; ctx.cancel.signaled = FALSE;
    ctx.content_pool = NULL;
    ctx.content_threads = args.threads;
    if(ctx.content_threads < 1) ctx.content_threads = 1;
    if(ctx.content_threads > MAX_THREADS) ctx.content_threads = MAX_THREADS;
    if(ctx.content_threads > 4) ctx.content_threads = 4;
    wcscpy_s(ctx.db_path, MAX_PATH, args.dbPath);
    size_t desired_queue = (size_t)args.threads * (size_t)args.batch;
    size_t min_queue = (size_t)args.threads * 4096;
    if(desired_queue < min_queue) desired_queue = min_queue;
    if(desired_queue < (size_t)(1u<<16)) desired_queue = (size_t)(1u<<16);
    LONG queue_pow2 = 1;
    while(queue_pow2 < (LONG)desired_queue && queue_pow2 < (1<<24)) queue_pow2 <<= 1;
    if(queue_pow2 < (LONG)desired_queue) queue_pow2 = (1<<24);
    if(!MPMC_Init(&ctx.queue, queue_pow2)){
        fwprintf(stderr, L"MPMC_Init failed\n");
        work_item_pool_destroy();
        db_close(db);
        return 1;
    }
    ctx.min_batch_size = 100;
    ctx.max_batch_size = 10000;
    if(ctx.batch_size < ctx.min_batch_size) ctx.batch_size = ctx.min_batch_size;
    if(ctx.batch_size > ctx.max_batch_size) ctx.batch_size = ctx.max_batch_size;
    ctx.idle_wait_ms = 25;
    ctx.consecutive_full_batches = 0;
    ctx.consecutive_idle_waits = 0;
    if(!writer_signal_init(&ctx.data_signal)){
        fwprintf(stderr, L"Failed to initialize writer signal\n");
        MPMC_Destroy(&ctx.queue);
        work_item_pool_destroy();
        db_close(db);
        return 1;
    }
    MPMC_SetOnPush(&ctx.queue, writer_queue_on_push, &ctx);
    DWORD computed_timeout = (DWORD)(args.threads * 25);
    if(computed_timeout < 50) computed_timeout = 50;
    ctx.push_timeout_ms = computed_timeout;
    PluginHost ph = { &ctx.queue, &ctx.cancel };
    Plugin_LoadAll(L"plugins", &ph);
    bloom_generator_init(args.dbPath);
    uintptr_t wh = _beginthreadex(NULL,0,(unsigned (__stdcall *)(void*))DbWriterThread,&ctx,0,NULL);
    HANDLE writer = (HANDLE)wh;
    if(!writer){
        fwprintf(stderr, L"Failed to start DbWriterThread\n");
        bloom_generator_shutdown();
        writer_signal_destroy(&ctx.data_signal);
        MPMC_Destroy(&ctx.queue);
        work_item_pool_destroy();
        Plugin_UnloadAll();
        db_close(db);
        return 1;
    }

    HANDLE drive_threads[26]; int drive_count=0;
    // Incremental index state
    IndexState st={0}; BOOL has_state = db_get_index_state(db, &st);
    if(!has_state || st.indexing_level == 0) st.indexing_level = INDEX_FULL_CONTENT;
    uint8_t current_sigs[26][32];
    ZeroMemory(current_sigs, sizeof(current_sigs));
    for(int i=0;i<26;i++){
        wchar_t root[8];
        swprintf(root,8,L"%c:\\", L'A'+i);
        compute_drive_signature(root, current_sigs[i]);
    }

    if(args.all_drives){
        DWORD mask = GetLogicalDrives();
        for(int i=0;i<26;i++){
            if(!(mask & (1u<<i))) continue;
            if(has_state && memcmp(current_sigs[i], st.drive_signatures[i], 32) == 0) continue;
            wchar_t root[8]; swprintf(root, 8, L"%c:\\", L'A'+i);
            UINT type = GetDriveTypeW(root);
            if(type==DRIVE_FIXED || type==DRIVE_REMOVABLE){
                struct { wchar_t root[8]; WriterCtx* ctx; BOOL use_ntfs; int threads; } *in = (struct { wchar_t root[8]; WriterCtx* ctx; BOOL use_ntfs; int threads; }*)malloc(sizeof(*in));
                wcscpy_s(in->root, 8, root);
                in->ctx=&ctx; in->use_ntfs=args.use_ntfs; in->threads=args.threads;
                uintptr_t dh = _beginthreadex(NULL,0,(unsigned (__stdcall *)(void*))scan_drive_thread,in,0,NULL);
                drive_threads[drive_count++] = (HANDLE)dh;
            }
        }
    } else {
        int di=-1;
        if(args.rootPath[0] >= L'A' && args.rootPath[0] <= L'Z') di = args.rootPath[0]-L'A';
        else if(args.rootPath[0] >= L'a' && args.rootPath[0] <= L'z') di = args.rootPath[0]-L'a';
        BOOL need_scan = TRUE;
        if(di>=0 && di<26 && has_state){
            if(memcmp(current_sigs[di], st.drive_signatures[di], 32) == 0) need_scan = FALSE;
        }
        if(need_scan){
            struct { wchar_t root[8]; WriterCtx* ctx; BOOL use_ntfs; int threads; } *in = (struct { wchar_t root[8]; WriterCtx* ctx; BOOL use_ntfs; int threads; }*)malloc(sizeof(*in));
            wcscpy_s(in->root, 8, args.rootPath);
            in->ctx=&ctx; in->use_ntfs=args.use_ntfs; in->threads=args.threads;
            uintptr_t dh = _beginthreadex(NULL,0,(unsigned (__stdcall *)(void*))scan_drive_thread,in,0,NULL);
            drive_threads[drive_count++] = (HANDLE)dh;
        }
    }

    WaitForMultipleObjects(drive_count, drive_threads, TRUE, INFINITE);
    for(int i=0;i<drive_count;i++) CloseHandle(drive_threads[i]);

    Plugin_ScanAll();

    if(args.tail_changes && !args.all_drives){
        HANDLE tailer = StartUSNTailer(args.rootPath, &ctx.queue, &ctx.cancel);
        if(tailer){
            wprintf(L"Tailing changes on %s. Press Ctrl+C to stop...\n", args.rootPath);
            Sleep(5000); // demo
        }
    }

    ctx.done = TRUE;
    push_with_backoff(&ctx.queue, NULL, INFINITE, "main thread sentinel");
    WaitForSingleObject(writer, INFINITE);
    CloseHandle(writer);

    if(!db_set_bulk_mode(db, FALSE, 0)){
        const DbError* derr = db_last_error(db);
        fwprintf(stderr, L"Warning: failed to disable bulk DB mode (err=%d: %hs)\n", derr?derr->detail:0, derr?derr->message:"");
    }
    if(db_begin_write(db)){
        if(!db_commit_write_ex(db, TRUE)){
            const DbError* derr = db_last_error(db);
            fwprintf(stderr, L"Warning: final database sync failed (err=%d: %hs)\n", derr?derr->detail:0, derr?derr->message:"");
        }
    } else {
        const DbError* derr = db_last_error(db);
        fwprintf(stderr, L"Warning: failed to begin final sync transaction (err=%d: %hs)\n", derr?derr->detail:0, derr?derr->message:"");
    }

    memcpy(st.drive_signatures, current_sigs, sizeof(current_sigs));
    FILETIME now; GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER uli; uli.LowPart = now.dwLowDateTime; uli.HighPart = now.dwHighDateTime;
    st.last_scan_time = uli.QuadPart;
    if(db_begin_write(db)){
        db_set_index_state(db, &st);
        db_commit_write(db);
    }

    const DbHeader* header = db_header(db);
    if(header){
        wprintf(L"Total records: %llu, strings: %llu, map: %llu MiB\n",
            (unsigned long long)header->record_count,
            (unsigned long long)header->string_count,
            (unsigned long long)(header->map_size_bytes/1024/1024));
    }
    writer_signal_destroy(&ctx.data_signal);
    Plugin_UnloadAll();
    bloom_generator_shutdown();
    db_close(db);
    MPMC_Destroy(&ctx.queue);
    work_item_pool_destroy();
    return 0;
}
