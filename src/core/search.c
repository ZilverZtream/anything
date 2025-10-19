
// search.c — fast query using trigram index + filters
#define _CRT_SECURE_NO_WARNINGS
#include "core/pch.h"
#include <process.h>
#include <inttypes.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#pragma comment(lib, "shlwapi.lib")

#ifndef _WIN32
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/file.h>
#include <limits.h>
#endif

#ifndef MDB_DBI_INVALID
#define MDB_DBI_INVALID ((MDB_dbi)~(unsigned)0)
#endif

static const uint8_t* bloom_readonly_base = NULL;
static size_t g_bloom_size = 0;
#define BLOOM_CACHE_CAP 1000

typedef struct {
    uint64_t string_id;
    uint64_t bloom_offset;
    uint32_t bloom_length;
    uint8_t  bloom_log2;
    uint8_t  hash_count;
    size_t   bloom_bytes;
    uint8_t* data;
    uint64_t stamp;
    BOOL     in_use;
} BloomCacheEntry;

static BloomCacheEntry g_bloom_cache[BLOOM_CACHE_CAP];
static size_t g_bloom_cache_count = 0;
static uint64_t g_bloom_cache_clock = 0;
static BOOL g_bloom_cache_initialized = FALSE;
static CRITICAL_SECTION g_bloom_cache_mu;
// Global database path for caching term results
static wchar_t g_db_path[MAX_LONG_PATH]={0};
static uint64_t g_db_generation = 0;

static inline size_t bloom_log2_to_bytes(uint8_t log2){
    if(log2 >= 8 && log2 <= 20){
        return (size_t)1u << log2;
    }
    return 0;
}

static BOOL string_value_parse(const MDB_val* value, MDB_val* text, StringMeta* meta_out){
    return db_string_value_parse(value, text, meta_out, NULL);
}

static size_t string_meta_bloom_bytes(const StringMeta* sm){
    if(!sm || sm->bloom_pending) return 0;
    if(sm->magic0 == STRING_META_MAGIC0 && sm->magic1 == STRING_META_MAGIC1){
        if(sm->bloom_log2){
            size_t bytes = bloom_log2_to_bytes(sm->bloom_log2);
            if(bytes) return bytes;
        }
    }
    if(sm->bloom_length == 0) return 0;
    if(sm->bloom_length == 2048 || sm->bloom_length == 4096 || sm->bloom_length == 8192){
        return sm->bloom_length;
    }
    return 8192;
}

static uint32_t string_meta_bloom_mask(const StringMeta* sm){
    size_t bytes = string_meta_bloom_bytes(sm);
    if(bytes == 0) return 0;
    size_t bits = bytes * 8;
    if(bits > UINT32_MAX) bits = UINT32_MAX;
    return (uint32_t)(bits - 1);
}

static void bloom_cache_init(void){
    if(!g_bloom_cache_initialized){
        InitializeCriticalSection(&g_bloom_cache_mu);
        g_bloom_cache_initialized = TRUE;
    }
}

static void bloom_cache_reset(void){
    if(!g_bloom_cache_initialized) return;
    EnterCriticalSection(&g_bloom_cache_mu);
    for(size_t i=0;i<BLOOM_CACHE_CAP;i++){
        if(g_bloom_cache[i].data){
            free(g_bloom_cache[i].data);
            g_bloom_cache[i].data = NULL;
        }
        g_bloom_cache[i].in_use = FALSE;
        g_bloom_cache[i].stamp = 0;
        g_bloom_cache[i].bloom_bytes = 0;
    }
    g_bloom_cache_count = 0;
    g_bloom_cache_clock = 0;
    LeaveCriticalSection(&g_bloom_cache_mu);
}

static void bloom_cache_shutdown(void){
    if(!g_bloom_cache_initialized) return;
    bloom_cache_reset();
    DeleteCriticalSection(&g_bloom_cache_mu);
    g_bloom_cache_initialized = FALSE;
}

static uint8_t* bloom_cache_get(uint64_t string_id, const StringMeta* meta, size_t* out_len){
    if(!meta || meta->hash_count == 0 || meta->bloom_length == 0) return NULL;
    size_t bloom_bytes = string_meta_bloom_bytes(meta);
    if(bloom_bytes == 0) return NULL;
    bloom_cache_init();
    EnterCriticalSection(&g_bloom_cache_mu);
    BloomCacheEntry* slot = NULL;
    for(size_t i=0;i<g_bloom_cache_count;i++){
        BloomCacheEntry* e = &g_bloom_cache[i];
        if(e->in_use && e->string_id == string_id){
            if(e->bloom_offset == meta->bloom_offset && e->bloom_length == meta->bloom_length && e->bloom_log2 == meta->bloom_log2){
                e->stamp = ++g_bloom_cache_clock;
                if(out_len) *out_len = e->bloom_bytes;
                uint8_t* data = e->data;
                LeaveCriticalSection(&g_bloom_cache_mu);
                return data;
            }
            slot = e;
            break;
        }
        if(!e->in_use && !slot){
            slot = e;
        }
    }
    if(!slot){
        if(g_bloom_cache_count < BLOOM_CACHE_CAP){
            slot = &g_bloom_cache[g_bloom_cache_count++];
        } else {
            size_t victim = 0;
            uint64_t best_stamp = UINT64_MAX;
            for(size_t i=0;i<BLOOM_CACHE_CAP;i++){
                if(!g_bloom_cache[i].in_use){
                    victim = i;
                    break;
                }
                if(g_bloom_cache[i].stamp < best_stamp){
                    best_stamp = g_bloom_cache[i].stamp;
                    victim = i;
                }
            }
            slot = &g_bloom_cache[victim];
        }
    }
    if(slot->data && slot->bloom_bytes != bloom_bytes){
        uint8_t* resized = (uint8_t*)realloc(slot->data, bloom_bytes);
        if(!resized){
            LeaveCriticalSection(&g_bloom_cache_mu);
            return NULL;
        }
        slot->data = resized;
    } else if(!slot->data){
        slot->data = (uint8_t*)malloc(bloom_bytes);
        if(!slot->data){
            LeaveCriticalSection(&g_bloom_cache_mu);
            return NULL;
        }
    }
    const uint8_t* encoded = bloom_readonly_base + meta->bloom_offset;
    if(meta->bloom_length == bloom_bytes){
        memcpy(slot->data, encoded, bloom_bytes);
    } else if(!bloom_packbits_decompress(encoded, meta->bloom_length, slot->data, bloom_bytes)){
        LeaveCriticalSection(&g_bloom_cache_mu);
        return NULL;
    }
    slot->string_id = string_id;
    slot->bloom_offset = meta->bloom_offset;
    slot->bloom_length = meta->bloom_length;
    slot->bloom_log2 = meta->bloom_log2;
    slot->hash_count = meta->hash_count;
    slot->bloom_bytes = bloom_bytes;
    slot->stamp = ++g_bloom_cache_clock;
    slot->in_use = TRUE;
    if(out_len) *out_len = bloom_bytes;
    uint8_t* result = slot->data;
    LeaveCriticalSection(&g_bloom_cache_mu);
    return result;
}

#include "anything/database.h"
#include "anything/anything.h"
#include "anything/util.h"
#include "../../third_party/lmdb/lmdb.h"
#include "anything/enterprise.h"
#include "anything/config.h"

#ifdef HAS_PCRE2
#include <pcre2.h>
#endif

#ifdef _WIN32
static inline uint64_t ticks(void){ LARGE_INTEGER t; QueryPerformanceCounter(&t); return (uint64_t)t.QuadPart; }
static double to_ms(uint64_t dt){ static double freq = 0; if(freq==0){ LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq=(double)f.QuadPart; } return 1000.0 * (double)dt / freq; }
#else
static inline uint64_t ticks(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return (uint64_t)ts.tv_sec*1000000000ull + ts.tv_nsec; }
static double to_ms(uint64_t dt){ return (double)dt / 1000000.0; }
#endif

#ifndef _WIN32
// Provide Win32-style critical section wrappers on POSIX systems
typedef pthread_mutex_t CRITICAL_SECTION;
static void InitializeCriticalSection(CRITICAL_SECTION* cs){ pthread_mutex_init(cs,NULL); }
static void DeleteCriticalSection(CRITICAL_SECTION* cs){ pthread_mutex_destroy(cs); }
static void EnterCriticalSection(CRITICAL_SECTION* cs){ pthread_mutex_lock(cs); }
static void LeaveCriticalSection(CRITICAL_SECTION* cs){ pthread_mutex_unlock(cs); }
#endif

// ---- Lightweight IdMap --------------------------------------------------
typedef struct {
    uint32_t* keys;    // rec_id
    uint16_t* vals;    // (stage<<8)|confidence
    size_t cap;
    size_t n;
} IdMap;

static void idmap_init(IdMap* m, size_t initial){
    m->cap = 1;
    while(m->cap < initial) m->cap <<= 1;
    m->keys = (uint32_t*)malloc(m->cap * sizeof(uint32_t));
    m->vals = (uint16_t*)malloc(m->cap * sizeof(uint16_t));
    for(size_t i=0;i<m->cap;i++){ m->keys[i]=0xFFFFFFFFu; m->vals[i]=0; }
    m->n = 0;
}

static void idmap_free(IdMap* m){
    free(m->keys); free(m->vals); m->keys=NULL; m->vals=NULL; m->cap=m->n=0;
}

static BOOL idmap_get(IdMap* m, uint32_t key, uint16_t* out){
    size_t mask = m->cap - 1; size_t i = key & mask;
    for(;;){
        uint32_t k = m->keys[i];
        if(k == 0xFFFFFFFFu) return FALSE;
        if(k == key){ if(out) *out = m->vals[i]; return TRUE; }
        i = (i + 1) & mask;
    }
}

static void idmap_grow(IdMap* m){
    size_t oldcap = m->cap; uint32_t* oldk = m->keys; uint16_t* oldv = m->vals;
    m->cap <<= 1; m->keys = (uint32_t*)malloc(m->cap*sizeof(uint32_t));
    m->vals = (uint16_t*)malloc(m->cap*sizeof(uint16_t));
    for(size_t i=0;i<m->cap;i++){ m->keys[i]=0xFFFFFFFFu; m->vals[i]=0; }
    m->n = 0;
    for(size_t i=0;i<oldcap;i++) if(oldk[i]!=0xFFFFFFFFu){
        size_t mask=m->cap-1; size_t j=oldk[i]&mask;
        while(m->keys[j]!=0xFFFFFFFFu) j=(j+1)&mask;
        m->keys[j]=oldk[i]; m->vals[j]=oldv[i]; m->n++;
    }
    free(oldk); free(oldv);
}

static void idmap_set(IdMap* m, uint32_t key, uint16_t val){
    if(m->n * 2 >= m->cap) idmap_grow(m);
    size_t mask = m->cap - 1; size_t i = key & mask;
    for(;;){
        uint32_t k = m->keys[i];
        if(k == 0xFFFFFFFFu){ m->keys[i]=key; m->vals[i]=val; m->n++; return; }
        if(k == key){ m->vals[i]=val; return; }
        i = (i + 1) & mask;
    }
}

// ---- Progressive search state -------------------------------------------
typedef struct {
    uint32_t rec_id;
    uint8_t  confidence;   // 0..100
    uint8_t  stage;        // 0=name,1=meta,2=content
} ProgHit; // consumer must free instances popped from ps->out

typedef struct {
    CRITICAL_SECTION mu;
    IdMap best;            // rec_id -> (stage<<8)|confidence
    MPMCQueue* out;        // output queue to UI
    BOOL done[3];          // completion flags per stage
} ProgState;

void prog_state_init(ProgState* ps, MPMCQueue* out){
    InitializeCriticalSection(&ps->mu);
    idmap_init(&ps->best, 1024);
    ps->out = out;
    ps->done[0]=ps->done[1]=ps->done[2]=FALSE;
}

void prog_state_release(ProgState* ps){
    if(ps->out){
        void* p;
        while(MPMC_Pop(ps->out, &p)){
            free(p);
        }
    }
    idmap_free(&ps->best);
    DeleteCriticalSection(&ps->mu);
}

void prog_submit(ProgState* ps, const uint32_t* ids, size_t n, uint8_t stage, uint8_t conf){
    EnterCriticalSection(&ps->mu);
    for(size_t i=0;i<n;i++){
        uint32_t id = ids[i];
        uint16_t prev;
        BOOL have = idmap_get(&ps->best, id, &prev);
        uint8_t pst = have ? (uint8_t)(prev>>8) : 0;
        uint8_t pconf = have ? (uint8_t)(prev & 0xFF) : 0;
        if(!have || stage>pst || (stage==pst && conf>pconf)){
            idmap_set(&ps->best, id, (uint16_t)((stage<<8)|conf));
            ProgHit* h = (ProgHit*)malloc(sizeof(ProgHit)); // ownership passed to consumer
            h->rec_id = id; h->confidence = conf; h->stage = stage;
            MPMC_Push(ps->out, h);
        }
    }
    LeaveCriticalSection(&ps->mu);
}

void prog_mark_done(ProgState* ps, uint8_t stage){
    if(stage<3){
        EnterCriticalSection(&ps->mu);
        ps->done[stage] = TRUE;
        LeaveCriticalSection(&ps->mu);
    }
}

#ifndef THREAD_LOCAL
#if defined(_MSC_VER)
#define THREAD_LOCAL __declspec(thread)
#else
#define THREAD_LOCAL __thread
#endif
#endif

#ifdef _WIN32
static HANDLE bloom_mapping = NULL;
#else
static int bloom_fd = -1;
#endif

// Magic marker and version for cache files so we can detect incompatible
// formats and upgrade transparently.
#define CACHE_MAGIC   0xCACEF00D
#ifdef _WIN32
static BOOL open_bloom(const wchar_t* dbPath){
    wchar_t bp[MAX_LONG_PATH]; swprintf(bp, MAX_LONG_PATH, L"%s\\bloom.dat", dbPath);
    wchar_t lp[MAX_LONG_PATH]; make_long_path(bp, lp, MAX_LONG_PATH);
    HANDLE f = CreateFileW(lp, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if(f==INVALID_HANDLE_VALUE) return FALSE;
    LARGE_INTEGER sz; GetFileSizeEx(f,&sz); g_bloom_size = sz.QuadPart;
    bloom_mapping = CreateFileMappingW(f, NULL, PAGE_READONLY, 0, 0, NULL);
    CloseHandle(f);
    if(!bloom_mapping) return FALSE;
    bloom_readonly_base = (const uint8_t*)MapViewOfFile(bloom_mapping, FILE_MAP_READ, 0,0,0);
    if(!bloom_readonly_base){ CloseHandle(bloom_mapping); bloom_mapping=NULL; return FALSE; }
    bloom_cache_reset();
    return TRUE;
}
static void close_bloom(void){
    if(bloom_readonly_base) UnmapViewOfFile(bloom_readonly_base);
    bloom_readonly_base=NULL;
    if(bloom_mapping) CloseHandle(bloom_mapping);
    bloom_mapping=NULL;
    bloom_cache_shutdown();
}

static BOOL bloom_packbits_decompress(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_len){
    if(!src || !dst) return FALSE;
    size_t i=0, o=0;
    while(i < src_len && o < dst_len){
        int8_t header = (int8_t)src[i++];
        if(header >= 0){
            size_t count = (size_t)header + 1;
            if(i + count > src_len || o + count > dst_len) return FALSE;
            memcpy(dst + o, src + i, count);
            i += count;
            o += count;
        } else if(header != -128){
            size_t count = (size_t)(1 - header);
            if(i >= src_len || o + count > dst_len) return FALSE;
            uint8_t value = src[i++];
            memset(dst + o, value, count);
            o += count;
        }
    }
    return o == dst_len;
}

#else
static BOOL open_bloom(const wchar_t* dbPath){
    char bp[MAX_PATH];
    wcstombs(bp, dbPath, MAX_PATH);
    char full[MAX_PATH];
    snprintf(full, MAX_PATH, "%s/bloom.dat", bp);
    bloom_fd = open(full, O_RDONLY);
    if(bloom_fd < 0) return FALSE;
    struct stat st;
    if(fstat(bloom_fd, &st) != 0){ close(bloom_fd); bloom_fd = -1; return FALSE; }
    g_bloom_size = (size_t)st.st_size;
    bloom_readonly_base = (const uint8_t*)mmap(NULL, g_bloom_size, PROT_READ, MAP_SHARED, bloom_fd, 0);
    if(bloom_readonly_base == MAP_FAILED){ close(bloom_fd); bloom_fd = -1; bloom_readonly_base = NULL; return FALSE; }
    bloom_cache_reset();
    return TRUE;
}
static void close_bloom(void){
    if(bloom_readonly_base && bloom_readonly_base != MAP_FAILED) munmap((void*)bloom_readonly_base, g_bloom_size);
    bloom_readonly_base = NULL;
    if(bloom_fd != -1) close(bloom_fd);
    bloom_fd = -1;
    bloom_cache_shutdown();
}
#endif

static BOOL string_contains_lower_term(const MDB_val* text, const char* lower_term){
    if(!text || !lower_term) return FALSE;
    size_t len = text->mv_size;
    char* tmp = (char*)malloc(len + 1);
    if(!tmp) return FALSE;
    memcpy(tmp, text->mv_data, len);
    tmp[len] = 0;
    BOOL needs_transform = FALSE;
    for(size_t i=0;i<len;i++){
        unsigned char c = (unsigned char)tmp[i];
        if((c >= 'A' && c <= 'Z') || c == '_' || c == '-' || c == '.'){
            needs_transform = TRUE;
            break;
        }
    }
    if(needs_transform){
        lowercase_ascii(tmp, len);
        for(size_t i=0;i<len;i++){
            if(tmp[i]=='_' || tmp[i]=='-' || tmp[i]=='.') tmp[i]=' ';
        }
    }
    BOOL match = strstr(tmp, lower_term) != NULL;
    free(tmp);
    return match;
}

typedef struct {
    char* name_pattern;
    char* content_pattern;
    char* author_pattern;
    char* camera_pattern;
    char* lens_pattern;
    char* artist_pattern;
    char* album_pattern;
    char* title_pattern;
    char* ext_pattern;
    uint64_t size_min, size_max;
    uint64_t date_min_day, date_max_day;
    char* path_filter;   // utf8
    bool regex_mode;
    bool whole_word;
} SearchQuery;

typedef enum { TOK_TERM, TOK_AND, TOK_OR, TOK_NOT, TOK_LPAREN, TOK_RPAREN } TokType;
typedef enum { TERM_NAME, TERM_AUTHOR, TERM_CAMERA, TERM_LENS, TERM_ARTIST, TERM_ALBUM, TERM_TITLE, TERM_EXT, TERM_CONTENT } TermType;
typedef struct { TokType type; TermType ttype; char* text; } Token;
typedef struct { Token* items; int n, cap; } TokenList;
static void tokenlist_init(TokenList* t){ t->items=NULL; t->n=t->cap=0; }
static void tokenlist_push(TokenList* t, Token tk){ if(t->n==t->cap){ t->cap=t->cap?t->cap*2:64; t->items=(Token*)realloc(t->items,t->cap*sizeof(Token)); } t->items[t->n++]=tk; }
static void tokenlist_free(TokenList* t){
    for(int i=0;i<t->n;i++){ if(t->items[i].type==TOK_TERM && t->items[i].text) free(t->items[i].text); }
    free(t->items); t->items=NULL; t->n=t->cap=0;
}

// Parallel search helpers
typedef SearchQuery Query;
typedef struct { uint64_t dummy; } Result;

// Forward declarations for helpers defined later in this file.
typedef struct IdVec IdVec;
static void idvec_init(IdVec* v);
static void idvec_free(IdVec* v);
static void galloping_intersect(IdVec* a, const IdVec* b);
static void union_inplace(IdVec* a, const IdVec* b);
static void difference_inplace(IdVec* a, const IdVec* b);
static uint64_t day_to_filetime(uint64_t day);
static void records_for_range(MDB_txn* txn, MDB_dbi dbi, uint64_t minv, uint64_t maxv, IdVec* out);
static void records_for_ext(MDB_txn* txn, MDB_dbi dbi_ext, const char* ext, IdVec* out);
static void records_for_name(MDB_txn* txn, MDB_dbi dbi_trigram, MDB_dbi dbi_fname, MDB_dbi dbi_strings, MDB_dbi dbi_smeta, const char* term, IdVec* out);
static void records_for_content(MDB_txn* txn, MDB_dbi dbi_trigram, MDB_dbi dbi_content, const char* term, IdVec* out);
static void records_for_author(MDB_txn* txn, MDB_dbi dbi_author, MDB_dbi dbi_strrev, const char* author, IdVec* out);
static void records_for_camera(MDB_txn* txn, MDB_dbi dbi_camera, MDB_dbi dbi_strrev, const char* camera, IdVec* out);
static void records_for_lens(MDB_txn* txn, MDB_dbi dbi_lens, MDB_dbi dbi_strrev, const char* lens, IdVec* out);
static void records_for_artist(MDB_txn* txn, MDB_dbi dbi_artist, MDB_dbi dbi_strrev, const char* artist, IdVec* out);
static void records_for_album(MDB_txn* txn, MDB_dbi dbi_album, MDB_dbi dbi_strrev, const char* album, IdVec* out);
static void records_for_title(MDB_txn* txn, MDB_dbi dbi_title, MDB_dbi dbi_strrev, const char* title, IdVec* out);

// Global progressive state used by stage wrappers.
static ProgState* g_prog_state = NULL;

// Stage 0: filename search and basic filters.
static int search_names(Query* q, Result* results){
    (void)results;
    if(!g_db_path[0]) return 0;
    MDB_env* env=NULL; MDB_txn* txn=NULL;
    MDB_dbi dbi_fname, dbi_trigram, dbi_strings, dbi_smeta, dbi_ext, dbi_size, dbi_mtime;
    if(mdb_env_create(&env)!=0) return 0;
    mdb_env_set_maxdbs(env,64);
    char u8db[MAX_LONG_PATH*3]; to_utf8(g_db_path, u8db, sizeof(u8db));
    if(mdb_env_open(env,u8db,MDB_RDONLY,0664)!=0){ mdb_env_close(env); return 0; }
    if(mdb_txn_begin(env,NULL,MDB_RDONLY,&txn)!=0){ mdb_env_close(env); return 0; }
    int rc = 0;
    if((rc = mdb_dbi_open(txn,"filename_index",0,&dbi_fname))!=0) goto fail;
    if((rc = mdb_dbi_open(txn,"trigram_index",0,&dbi_trigram))!=0) goto fail;
    if((rc = mdb_dbi_open(txn,"strings",0,&dbi_strings))!=0) goto fail;
    dbi_smeta = MDB_DBI_INVALID;
    rc = mdb_dbi_open(txn,"string_meta",0,&dbi_smeta);
    if(rc == MDB_NOTFOUND){
        dbi_smeta = MDB_DBI_INVALID;
    } else if(rc != 0){
        goto fail;
    }
    if((rc = mdb_dbi_open(txn,"extension_index",0,&dbi_ext))!=0) goto fail;
    if((rc = mdb_dbi_open(txn,"size_index",0,&dbi_size))!=0) goto fail;
    if((rc = mdb_dbi_open(txn,"mtime_index",0,&dbi_mtime))!=0) goto fail;

    IdVec base; idvec_init(&base);
    BOOL have_base = FALSE;
    if(q->name_pattern){
        if(load_stage_cache(0, q, &base)){
            have_base = TRUE;
        } else {
            if(!try_load_term_cache(TERM_NAME, q->name_pattern, &base)){
                records_for_name(txn, dbi_trigram, dbi_fname, dbi_strings, dbi_smeta, q->name_pattern, &base);
                sort_unique(&base);
                save_term_cache(TERM_NAME, q->name_pattern, &base);
            } else {
                sort_unique(&base);
            }
            save_stage_cache(0, q, &base);
            have_base = TRUE;
        }
    }

    IdVec ids; idvec_init(&ids);
    if(have_base && base.n>0){
        ids.ids = (uint64_t*)malloc(base.n * sizeof(uint64_t));
        if(ids.ids){
            memcpy(ids.ids, base.ids, base.n * sizeof(uint64_t));
            ids.n = ids.cap = base.n;
        }
    }

    if(q->ext_pattern){
        IdVec ext; idvec_init(&ext);
        if(!try_load_term_cache(TERM_EXT, q->ext_pattern, &ext)){
            records_for_ext(txn, dbi_ext, q->ext_pattern, &ext);
            sort_unique(&ext);
            save_term_cache(TERM_EXT, q->ext_pattern, &ext);
        } else {
            sort_unique(&ext);
        }
        if(ids.n>0){ galloping_intersect(&ids,&ext); idvec_free(&ext); }
        else { ids = ext; }
    }
    if(q->size_min>0 || q->size_max<~0ULL){
        IdVec sz; idvec_init(&sz);
        records_for_range(txn, dbi_size, q->size_min, q->size_max, &sz);
        if(ids.n>0){ galloping_intersect(&ids,&sz); idvec_free(&sz); }
        else { ids = sz; }
    }
    if(q->date_min_day>0 || q->date_max_day<~0ULL){
        uint64_t minft = day_to_filetime(q->date_min_day);
        uint64_t maxft = day_to_filetime(q->date_max_day+1) - 1;
        IdVec dt; idvec_init(&dt);
        records_for_range(txn, dbi_mtime, minft, maxft, &dt);
        if(ids.n>0){ galloping_intersect(&ids,&dt); idvec_free(&dt); }
        else { ids = dt; }
    }
    idvec_free(&base);

    size_t n = ids.n;
    if(g_prog_state && n>0){
        uint32_t* out=(uint32_t*)malloc(n*sizeof(uint32_t));
        for(size_t i=0;i<n;i++) out[i]=(uint32_t)ids.ids[i];
        prog_submit(g_prog_state,out,n,0,60);
        free(out);
    }
    prog_mark_done(g_prog_state,0);
    idvec_free(&ids);
    mdb_txn_abort(txn); mdb_env_close(env);
    return (int)n;
fail:
    mdb_txn_abort(txn);
    mdb_env_close(env);
    return 0;
}

// Stage 1: metadata indexes (author/camera/etc.).
static int search_metadata(Query* q, Result* results){
    (void)results;
    if(!g_db_path[0]){ prog_mark_done(g_prog_state,1); return 0; }
    MDB_env* env=NULL; MDB_txn* txn=NULL;
    MDB_dbi dbi_author, dbi_camera, dbi_lens, dbi_artist, dbi_album, dbi_title, dbi_strrev;
    if(mdb_env_create(&env)!=0) { prog_mark_done(g_prog_state,1); return 0; }
    mdb_env_set_maxdbs(env,64);
    char u8db[MAX_LONG_PATH*3]; to_utf8(g_db_path, u8db, sizeof(u8db));
    if(mdb_env_open(env,u8db,MDB_RDONLY,0664)!=0){ mdb_env_close(env); prog_mark_done(g_prog_state,1); return 0; }
    if(mdb_txn_begin(env,NULL,MDB_RDONLY,&txn)!=0){ mdb_env_close(env); prog_mark_done(g_prog_state,1); return 0; }
    if(mdb_dbi_open(txn,"author_index",0,&dbi_author)!=0 ||
       mdb_dbi_open(txn,"camera_index",0,&dbi_camera)!=0 ||
       mdb_dbi_open(txn,"lens_index",0,&dbi_lens)!=0 ||
       mdb_dbi_open(txn,"artist_index",0,&dbi_artist)!=0 ||
       mdb_dbi_open(txn,"album_index",0,&dbi_album)!=0 ||
       mdb_dbi_open(txn,"title_index",0,&dbi_title)!=0 ||
       mdb_dbi_open(txn,"strrev",0,&dbi_strrev)!=0){
        mdb_txn_abort(txn); mdb_env_close(env); prog_mark_done(g_prog_state,1); return 0;
    }

    BOOL wants_cache = q->author_pattern || q->camera_pattern || q->lens_pattern ||
                       q->artist_pattern || q->album_pattern || q->title_pattern;
    IdVec ids; idvec_init(&ids);
    BOOL from_cache = FALSE;
    if(wants_cache && load_stage_cache(1, q, &ids)){
        from_cache = TRUE;
    }

    if(!from_cache){
        if(q->author_pattern){
            IdVec tmp; idvec_init(&tmp);
            if(!try_load_term_cache(TERM_AUTHOR, q->author_pattern, &tmp)){
                records_for_author(txn, dbi_author, dbi_strrev, q->author_pattern, &tmp);
                sort_unique(&tmp);
                save_term_cache(TERM_AUTHOR, q->author_pattern, &tmp);
            } else {
                sort_unique(&tmp);
            }
            union_inplace(&ids,&tmp); idvec_free(&tmp);
        }
        if(q->camera_pattern){
            IdVec tmp; idvec_init(&tmp);
            if(!try_load_term_cache(TERM_CAMERA, q->camera_pattern, &tmp)){
                records_for_camera(txn, dbi_camera, dbi_strrev, q->camera_pattern, &tmp);
                sort_unique(&tmp);
                save_term_cache(TERM_CAMERA, q->camera_pattern, &tmp);
            } else { sort_unique(&tmp); }
            union_inplace(&ids,&tmp); idvec_free(&tmp);
        }
        if(q->lens_pattern){
            IdVec tmp; idvec_init(&tmp);
            if(!try_load_term_cache(TERM_LENS, q->lens_pattern, &tmp)){
                records_for_lens(txn, dbi_lens, dbi_strrev, q->lens_pattern, &tmp);
                sort_unique(&tmp);
                save_term_cache(TERM_LENS, q->lens_pattern, &tmp);
            } else { sort_unique(&tmp); }
            union_inplace(&ids,&tmp); idvec_free(&tmp);
        }
        if(q->artist_pattern){
            IdVec tmp; idvec_init(&tmp);
            if(!try_load_term_cache(TERM_ARTIST, q->artist_pattern, &tmp)){
                records_for_artist(txn, dbi_artist, dbi_strrev, q->artist_pattern, &tmp);
                sort_unique(&tmp);
                save_term_cache(TERM_ARTIST, q->artist_pattern, &tmp);
            } else { sort_unique(&tmp); }
            union_inplace(&ids,&tmp); idvec_free(&tmp);
        }
        if(q->album_pattern){
            IdVec tmp; idvec_init(&tmp);
            if(!try_load_term_cache(TERM_ALBUM, q->album_pattern, &tmp)){
                records_for_album(txn, dbi_album, dbi_strrev, q->album_pattern, &tmp);
                sort_unique(&tmp);
                save_term_cache(TERM_ALBUM, q->album_pattern, &tmp);
            } else { sort_unique(&tmp); }
            union_inplace(&ids,&tmp); idvec_free(&tmp);
        }
        if(q->title_pattern){
            IdVec tmp; idvec_init(&tmp);
            if(!try_load_term_cache(TERM_TITLE, q->title_pattern, &tmp)){
                records_for_title(txn, dbi_title, dbi_strrev, q->title_pattern, &tmp);
                sort_unique(&tmp);
                save_term_cache(TERM_TITLE, q->title_pattern, &tmp);
            } else { sort_unique(&tmp); }
            union_inplace(&ids,&tmp); idvec_free(&tmp);
        }
        if(ids.n>0) sort_unique(&ids);
        if(wants_cache) save_stage_cache(1, q, &ids);
    }

    size_t n = ids.n;
    if(g_prog_state && n>0){
        uint32_t* out=(uint32_t*)malloc(n*sizeof(uint32_t));
        for(size_t i=0;i<n;i++) out[i]=(uint32_t)ids.ids[i];
        prog_submit(g_prog_state,out,n,1,75);
        free(out);
    }
    prog_mark_done(g_prog_state,1);
    idvec_free(&ids);
    mdb_txn_abort(txn); mdb_env_close(env);
    return (int)n;
}

// Stage 2: content search (trigram/regex).
static int search_content(Query* q, Result* results){
    (void)results;
    if(!g_db_path[0]){ prog_mark_done(g_prog_state,2); return 0; }
    MDB_env* env=NULL; MDB_txn* txn=NULL;
    MDB_dbi dbi_trigram, dbi_content;
    if(mdb_env_create(&env)!=0){ prog_mark_done(g_prog_state,2); return 0; }
    mdb_env_set_maxdbs(env,64);
    char u8db[MAX_LONG_PATH*3]; to_utf8(g_db_path, u8db, sizeof(u8db));
    if(mdb_env_open(env,u8db,MDB_RDONLY,0664)!=0){ mdb_env_close(env); prog_mark_done(g_prog_state,2); return 0; }
    if(mdb_txn_begin(env,NULL,MDB_RDONLY,&txn)!=0){ mdb_env_close(env); prog_mark_done(g_prog_state,2); return 0; }
    if(mdb_dbi_open(txn,"trigram_index",0,&dbi_trigram)!=0 ||
       mdb_dbi_open(txn,"content_index",0,&dbi_content)!=0){
        mdb_txn_abort(txn); mdb_env_close(env); prog_mark_done(g_prog_state,2); return 0;
    }

    IdVec ids; idvec_init(&ids);
    if(q->content_pattern){
        if(!load_stage_cache(2, q, &ids)){
            if(!try_load_term_cache(TERM_CONTENT, q->content_pattern, &ids)){
                records_for_content(txn, dbi_trigram, dbi_content, q->content_pattern, &ids);
                sort_unique(&ids);
                save_term_cache(TERM_CONTENT, q->content_pattern, &ids);
            } else {
                sort_unique(&ids);
            }
            save_stage_cache(2, q, &ids);
        }
    }

    size_t n = ids.n;
    if(g_prog_state && n>0){
        uint32_t* out=(uint32_t*)malloc(n*sizeof(uint32_t));
        for(size_t i=0;i<n;i++) out[i]=(uint32_t)ids.ids[i];
        prog_submit(g_prog_state,out,n,2,95);
        free(out);
    }
    prog_mark_done(g_prog_state,2);
    idvec_free(&ids);
    mdb_txn_abort(txn); mdb_env_close(env);
    return (int)n;
}

typedef int (*SearchFn)(Query*, Result*);

typedef struct {
    SearchFn fn;
    Query* q;
    Result* results;
} SearchTask;

typedef struct {
    SearchTask* tasks;
    int task_count;
#ifdef _WIN32
    volatile LONG next;
#else
    volatile int next;
#endif
} TaskQueue;

#ifdef _WIN32
static unsigned __stdcall pool_worker(void* param){
    TaskQueue* q = (TaskQueue*)param;
    for(;;){
        LONG i = InterlockedIncrement(&q->next) - 1;
        if(i >= q->task_count) break;
        SearchTask t = q->tasks[i];
        t.fn(t.q, t.results);
    }
    return 0;
}
#else
static void* pool_worker(void* param){
    TaskQueue* q = (TaskQueue*)param;
    for(;;){
        int i = __sync_fetch_and_add(&q->next, 1);
        if(i >= q->task_count) break;
        SearchTask t = q->tasks[i];
        t.fn(t.q, t.results);
    }
    return NULL;
}
#endif

static unsigned get_hw_threads(void){
#ifdef _WIN32
    SYSTEM_INFO si; GetSystemInfo(&si);
    return si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (unsigned)(n > 0 ? n : 1);
#endif
}

static double g_stage1_ms_avg = 0.0;

#define STAGE_KEY_BUF 1024

static void build_stage_key(const SearchQuery* q, int stage, char* out, size_t len){
    const char* empty = "";
    switch(stage){
        case 0:
            snprintf(out, len, "stage0|name=%s|regex=%d|whole=%d",
                     q->name_pattern ? q->name_pattern : empty,
                     q->regex_mode ? 1 : 0,
                     q->whole_word ? 1 : 0);
            break;
        case 1:
            snprintf(out, len, "stage1|author=%s|camera=%s|lens=%s|artist=%s|album=%s|title=%s",
                     q->author_pattern ? q->author_pattern : empty,
                     q->camera_pattern ? q->camera_pattern : empty,
                     q->lens_pattern ? q->lens_pattern : empty,
                     q->artist_pattern ? q->artist_pattern : empty,
                     q->album_pattern ? q->album_pattern : empty,
                     q->title_pattern ? q->title_pattern : empty);
            break;
        default:
            snprintf(out, len, "stage2|content=%s|regex=%d|whole=%d",
                     q->content_pattern ? q->content_pattern : empty,
                     q->regex_mode ? 1 : 0,
                     q->whole_word ? 1 : 0);
            break;
    }
}

static BOOL load_stage_cache(int stage, const SearchQuery* q, IdVec* out){
    if(!g_db_path[0]) return FALSE;
    char key[STAGE_KEY_BUF];
    build_stage_key(q, stage, key, sizeof(key));
    return try_load_cache_internal(g_db_path, key, CACHE_KIND_STAGE, (uint16_t)stage, out);
}

static void save_stage_cache(int stage, const SearchQuery* q, const IdVec* ids){
    if(!g_db_path[0]) return;
    char key[STAGE_KEY_BUF];
    build_stage_key(q, stage, key, sizeof(key));
    save_cache_internal(g_db_path, key, CACHE_KIND_STAGE, (uint16_t)stage, ids);
}

#define SMALL_POSTINGS_MAX 4096
#define TINY_QUERY_MS 2.0

// Adaptive search that runs stages serially for cheap queries and
// leverages a small thread pool for heavier ones.
static void progressive_search(Query* q, Result* results, ProgState* ps){
    g_prog_state = ps;
    unsigned hw = get_hw_threads();
    unsigned pool_size = hw < 3 ? hw : 3;

    uint64_t t0 = ticks();
    int id_count = search_names(q, results);
    double ms = to_ms(ticks() - t0);

    const double ALPHA = 0.2;
    if(g_stage1_ms_avg==0.0) g_stage1_ms_avg = ms;
    else g_stage1_ms_avg = g_stage1_ms_avg*(1.0-ALPHA) + ms*ALPHA;

    BOOL cheap = (id_count <= SMALL_POSTINGS_MAX) && !q->regex_mode && ms < TINY_QUERY_MS;
    if(pool_size <= 1 || cheap || g_stage1_ms_avg < 1.0){
        // Run subsequent stages serially to avoid thread overhead
        search_metadata(q, results);
        search_content(q, results);
        return;
    }

    // Run metadata and content stages in parallel
    SearchTask tasks[2] = {
        {search_metadata, q, results},
        {search_content,  q, results},
    };
    TaskQueue queue = { tasks, 2, 0 };
    unsigned workers = pool_size;
    if(workers > 2) workers = 2;

#ifdef _WIN32
    HANDLE threads[3];
    for(unsigned i=0;i<workers;i++){
        threads[i] = (HANDLE)_beginthreadex(NULL, 0, pool_worker, &queue, 0, NULL);
    }
    WaitForMultipleObjects(workers, threads, TRUE, INFINITE);
    for(unsigned i=0;i<workers;i++){
        CloseHandle(threads[i]);
    }
#else
    pthread_t threads[3];
    for(unsigned i=0;i<workers;i++){
        pthread_create(&threads[i], NULL, pool_worker, &queue);
    }
    for(unsigned i=0;i<workers;i++){
        pthread_join(threads[i], NULL);
    }
#endif
    g_prog_state = NULL;
}

// Helpers for progressive search -------------------------------------------------

static int cmp_u32(const void* a, const void* b){
    uint32_t ua = *(const uint32_t*)a, ub = *(const uint32_t*)b;
    return ua < ub ? -1 : (ua > ub);
}

static void sort_unique(uint32_t* ids, size_t* n){
    if(!ids || *n==0) return;
    qsort(ids, *n, sizeof(uint32_t), cmp_u32);
    size_t w=0; uint32_t prev=0;
    for(size_t i=0;i<*n;i++){
        uint32_t v = ids[i];
        if(i==0 || v!=prev){ ids[w++]=v; prev=v; }
    }
    *n = w;
}

static uint32_t* intersect_sorted(const uint32_t* a, size_t na, const uint32_t* b, size_t nb, size_t* out_n){
    size_t i=0,j=0,k=0; size_t cap = na < nb ? na : nb;
    uint32_t* out = (uint32_t*)malloc(cap * sizeof(uint32_t));
    if(!out){ *out_n=0; return NULL; }
    while(i<na && j<nb){
        uint32_t va=a[i], vb=b[j];
        if(va==vb){ out[k++]=va; i++; j++; }
        else if(va<vb) i++; else j++;
    }
    *out_n = k;
    return out;
}

static void results_to_ids(const Result* res, size_t n, uint32_t** out_ids, size_t* out_n){
    if(n==0){ *out_ids=NULL; *out_n=0; return; }
    uint32_t* ids=(uint32_t*)malloc(n*sizeof(uint32_t));
    if(!ids){ *out_ids=NULL; *out_n=0; return; }
    for(size_t i=0;i<n;i++) ids[i]=(uint32_t)res[i].dummy;
    *out_ids = ids; *out_n = n; sort_unique(*out_ids, out_n);
}

#define INTERSECT_EAGER_MAX 100000u

// Progressive version that reports early hits through ProgState
// (progressive_search removed; superseded by adaptive progressive_search above)

typedef struct Node{ int type; TermType ttype; char* text; struct Node* left; struct Node* right; } Node;
static void free_node(Node* n){ if(!n)return; free_node(n->left); free_node(n->right); if(n->type==TOK_TERM && n->text) free(n->text); free(n); }

static void usage(void){
    wprintf(L"search.exe --db <path> [--workers N] [--json] [--start-indexer|--pause-indexer] <terms and filters>\n");
}

static uint64_t parse_size(const char* s){
    // e.g., "100k", "5mb", "1g"
    char* end=NULL;
    double v = strtod(s, &end);
    uint64_t mult=1;
    if(end && *end){
        if(*end=='k'||*end=='K') mult=1024ULL;
        else if(*end=='m'||*end=='M') mult=1024ULL*1024ULL;
        else if(*end=='g'||*end=='G') mult=1024ULL*1024ULL*1024ULL;
    }
    return (uint64_t)(v*mult);
}

static uint64_t today_day(void){
    SYSTEMTIME st; GetSystemTime(&st);
    FILETIME ft; SystemTimeToFileTime(&st, &ft);
    ULARGE_INTEGER u; u.LowPart=ft.dwLowDateTime; u.HighPart=ft.dwHighDateTime;
    return filetime_days(u.QuadPart);
}

static uint64_t day_to_filetime(uint64_t day){
    const uint64_t TICKS_PER_DAY = 864000000000ULL;
    return day * TICKS_PER_DAY;
}

static BOOL parse_date(const char* s, uint64_t* out_day){
    if(_stricmp(s,"today")==0){ *out_day = today_day(); return TRUE; }
    if(_stricmp(s,"yesterday")==0){ *out_day = today_day()-1; return TRUE; }
    size_t n = strlen(s);
    if(n>1 && (s[n-1]=='d'||s[n-1]=='D')){
        int days = atoi(s);
        *out_day = today_day() - (uint64_t)days;
        return TRUE;
    }
    int y=0,m=0,d=1;
    if(sscanf(s,"%d-%d-%d",&y,&m,&d)>=2){
        SYSTEMTIME st = {0}; st.wYear=(WORD)y; st.wMonth=(WORD)m; st.wDay=(WORD)d;
        FILETIME ft; if(!SystemTimeToFileTime(&st,&ft)) return FALSE;
        ULARGE_INTEGER u; u.LowPart=ft.dwLowDateTime; u.HighPart=ft.dwHighDateTime;
        *out_day = filetime_days(u.QuadPart);
        return TRUE;
    }
    return FALSE;
}

static void json_escape_and_print(const char* s){
    for(const unsigned char* p=(const unsigned char*)s; *p; ++p){
        unsigned char c=*p;
        switch(c){
        case '\\': case '"': printf("\\%c", c); break;
        case '\b': printf("\\b"); break;
        case '\f': printf("\\f"); break;
        case '\n': printf("\\n"); break;
        case '\r': printf("\\r"); break;
        case '\t': printf("\\t"); break;
        default:
            if(c < 0x20) printf("\\u%04x", c);
            else putchar(c);
        }
    }
}

static void print_json_path(const char* dir, const char* name){
    putchar('"');
    json_escape_and_print(dir);
    printf("\\\\");
    json_escape_and_print(name);
    putchar('"');
}

static void output_error(bool json, const char* msg){
    if(json){
        printf("{\"error\":\"");
        json_escape_and_print(msg);
        printf("\"}\n");
    } else {
        fwprintf(stderr, L"%hs\n", msg);
    }
}

static int set_indexer_state(const wchar_t* dbPath, bool start, bool json){
    MDB_env* env;
    if(mdb_env_create(&env)!=0){ output_error(json, "env_create failed"); return 1; }
    char u8db[MAX_LONG_PATH*3]; to_utf8(dbPath, u8db, sizeof(u8db));
    if(mdb_env_open(env, u8db, 0, 0664)!=0){ mdb_env_close(env); output_error(json, "env_open failed"); return 1; }
    MDB_txn* txn;
    if(mdb_txn_begin(env, NULL, 0, &txn)!=0){ mdb_env_close(env); output_error(json, "txn_begin failed"); return 1; }
    MDB_dbi dbi_meta;
    if(mdb_dbi_open(txn, "meta", 0, &dbi_meta)!=0){ mdb_txn_abort(txn); mdb_env_close(env); output_error(json, "dbi_open failed"); return 1; }
    MDB_val k={.mv_data="index_state",.mv_size=11}, v;
    IndexState st; ZeroMemory(&st, sizeof(st));
    if(mdb_get(txn, dbi_meta, &k, &v)==0 && v.mv_size==sizeof(IndexState)){
        memcpy(&st, v.mv_data, sizeof(st));
    }
    st.indexing_level = start ? INDEX_FULL_CONTENT : 0;
    MDB_val nv={.mv_data=&st,.mv_size=sizeof(st)};
    if(mdb_put(txn, dbi_meta, &k, &nv, 0)!=0){ mdb_txn_abort(txn); mdb_env_close(env); output_error(json, "set index state failed"); return 1; }
    mdb_txn_commit(txn);
    mdb_env_close(env);
    if(json){
        printf("{\"status\":\"%s\"}\n", start?"started":"paused");
    } else {
        wprintf(L"Indexer %s\n", start?L"started":L"paused");
    }
    return 0;
}

static void add_logic_token(TokenList* toks, const char* s){
    if(!*s) return;
    if(_stricmp(s,"AND")==0){ tokenlist_push(toks,(Token){.type=TOK_AND}); return; }
    if(_stricmp(s,"OR")==0){ tokenlist_push(toks,(Token){.type=TOK_OR}); return; }
    if(_stricmp(s,"NOT")==0){ tokenlist_push(toks,(Token){.type=TOK_NOT}); return; }
    if(_strnicmp(s,"author:",7)==0){ tokenlist_push(toks,(Token){.type=TOK_TERM,.ttype=TERM_AUTHOR,.text=_strdup(s+7)}); return; }
    if(_strnicmp(s,"camera:",7)==0){ tokenlist_push(toks,(Token){.type=TOK_TERM,.ttype=TERM_CAMERA,.text=_strdup(s+7)}); return; }
    if(_strnicmp(s,"lens:",5)==0){ tokenlist_push(toks,(Token){.type=TOK_TERM,.ttype=TERM_LENS,.text=_strdup(s+5)}); return; }
    if(_strnicmp(s,"artist:",7)==0){ tokenlist_push(toks,(Token){.type=TOK_TERM,.ttype=TERM_ARTIST,.text=_strdup(s+7)}); return; }
    if(_strnicmp(s,"album:",6)==0){ tokenlist_push(toks,(Token){.type=TOK_TERM,.ttype=TERM_ALBUM,.text=_strdup(s+6)}); return; }
    if(_strnicmp(s,"title:",6)==0){ tokenlist_push(toks,(Token){.type=TOK_TERM,.ttype=TERM_TITLE,.text=_strdup(s+6)}); return; }
    if(_strnicmp(s,"ext:",4)==0){ tokenlist_push(toks,(Token){.type=TOK_TERM,.ttype=TERM_EXT,.text=_strdup(s+4)}); return; }
    if(_strnicmp(s,"content:",8)==0){ tokenlist_push(toks,(Token){.type=TOK_TERM,.ttype=TERM_CONTENT,.text=_strdup(s+8)}); return; }
    tokenlist_push(toks,(Token){.type=TOK_TERM,.ttype=TERM_NAME,.text=_strdup(s)});
}

static void parse_query(int argc, wchar_t** argv, wchar_t* dbPath, SearchQuery* q, TokenList* tokens){
    dbPath[0]=0; ZeroMemory(q,sizeof(*q));
    q->size_min=0; q->size_max=~0ULL;
    q->date_min_day=0; q->date_max_day=~0ULL;
    tokenlist_init(tokens);
    for(int i=1;i<argc;i++){
        if(wcscmp(argv[i], L"--db")==0 && i+1<argc){ wcscpy_s(dbPath, MAX_LONG_PATH, argv[++i]); continue; }
        if(wcscmp(argv[i], L"--start-indexer")==0 || wcscmp(argv[i], L"--pause-indexer")==0) continue;
        char u8[1024]; to_utf8(argv[i], u8, sizeof(u8));
        if(_strnicmp(u8,"size:",5)==0){
            const char* s=u8+5;
            if(s[0]=='>'||s[0]=='<'){
                BOOL gt=(s[0]=='>'); s++; uint64_t val=parse_size(s);
                if(gt){ q->size_min=val+1; } else { q->size_max=val-1; }
            } else {
                const char* dots=strstr(s,"..");
                if(dots){ char a[64]={0},b[64]={0}; strncpy(a,s,(size_t)(dots-s)); strcpy(b,dots+2); q->size_min=parse_size(a); q->size_max=parse_size(b); }
                else { q->size_min=q->size_max=parse_size(s); }
            }
        } else if(_strnicmp(u8,"dm:",3)==0){
            const char* s=u8+3;
            if(s[0]=='>'||s[0]=='<'){
                BOOL gt=(s[0]=='>'); s++; uint64_t day; if(parse_date(s,&day)){ if(gt){ q->date_min_day=day+1; } else { q->date_max_day=day-1; } }
            } else {
                const char* dots=strstr(s,"..");
                if(dots){ char a[64]={0},b[64]={0}; strncpy(a,s,(size_t)(dots-s)); strcpy(b,dots+2); uint64_t da,db; if(parse_date(a,&da)&&parse_date(b,&db)){ q->date_min_day=da; q->date_max_day=db; } }
                else { uint64_t d; if(parse_date(s,&d)){ q->date_min_day=d; q->date_max_day=d; } }
            }
        } else if(_strnicmp(u8,"path:",5)==0){
            q->path_filter=_strdup(u8+5); lowercase_ascii(q->path_filter,strlen(q->path_filter));
        } else if(_strnicmp(u8,"regex:",6)==0){
            q->regex_mode=true; q->name_pattern=_strdup(u8+6);
        } else if(_strnicmp(u8,"whole:",6)==0){
            q->whole_word=(_stricmp(u8+6,"yes")==0||_stricmp(u8+6,"true")==0);
        } else {
            // break possible parentheses
            char buf[1024]; int bi=0; for(size_t j=0; u8[j]; ++j){
                if(u8[j]=='(' || u8[j]==')'){
                    if(bi>0){ buf[bi]=0; add_logic_token(tokens, buf); bi=0; }
                    Token t={.type=(u8[j]=='(')?TOK_LPAREN:TOK_RPAREN}; tokenlist_push(tokens,t);
                } else {
                    buf[bi++]=u8[j];
                }
            }
            if(bi>0){ buf[bi]=0; add_logic_token(tokens, buf); }
        }
    }
    // choose first term of each type for scoring
    for(int i=0;i<tokens->n;i++){
        if(tokens->items[i].type!=TOK_TERM || !tokens->items[i].text) continue;
        if(!q->name_pattern && tokens->items[i].ttype==TERM_NAME) q->name_pattern=_strdup(tokens->items[i].text);
        if(!q->content_pattern && tokens->items[i].ttype==TERM_CONTENT) q->content_pattern=_strdup(tokens->items[i].text);
        if(!q->author_pattern && tokens->items[i].ttype==TERM_AUTHOR) q->author_pattern=_strdup(tokens->items[i].text);
        if(!q->camera_pattern && tokens->items[i].ttype==TERM_CAMERA) q->camera_pattern=_strdup(tokens->items[i].text);
        if(!q->lens_pattern && tokens->items[i].ttype==TERM_LENS) q->lens_pattern=_strdup(tokens->items[i].text);
        if(!q->artist_pattern && tokens->items[i].ttype==TERM_ARTIST) q->artist_pattern=_strdup(tokens->items[i].text);
        if(!q->album_pattern && tokens->items[i].ttype==TERM_ALBUM) q->album_pattern=_strdup(tokens->items[i].text);
        if(!q->title_pattern && tokens->items[i].ttype==TERM_TITLE) q->title_pattern=_strdup(tokens->items[i].text);
        if(!q->ext_pattern && tokens->items[i].ttype==TERM_EXT) q->ext_pattern=_strdup(tokens->items[i].text);
    }
}

static void free_search_query(SearchQuery* q){
    if(q->name_pattern) free(q->name_pattern);
    if(q->content_pattern) free(q->content_pattern);
    if(q->author_pattern) free(q->author_pattern);
    if(q->camera_pattern) free(q->camera_pattern);
    if(q->lens_pattern) free(q->lens_pattern);
    if(q->artist_pattern) free(q->artist_pattern);
    if(q->album_pattern) free(q->album_pattern);
    if(q->title_pattern) free(q->title_pattern);
    if(q->ext_pattern) free(q->ext_pattern);
    if(q->path_filter) free(q->path_filter);
}

typedef struct {
    uint64_t* ids;
    size_t n, cap;
} IdVec;
static void idvec_init(IdVec* v){ v->ids=NULL; v->n=v->cap=0; }
static void idvec_reserve(IdVec* v, size_t need){
    if(need <= v->cap) return;
    size_t cap = v->cap ? v->cap : 512;
    while(cap < need) cap <<= 1;
    uint64_t* ni = (uint64_t*)realloc(v->ids, cap * sizeof(uint64_t));
    if(!ni) return;
    v->ids = ni;
    v->cap = cap;
}
static void idvec_push(IdVec* v, uint64_t x){
    if(v->n==v->cap){
        size_t newcap = v->cap? v->cap*2:512;
        uint64_t* ni = (uint64_t*)realloc(v->ids,newcap*sizeof(uint64_t));
        if(!ni) return;
        v->ids = ni;
        v->cap = newcap;
    }
    v->ids[v->n++]=x;
}
static void idvec_free(IdVec* v){ free(v->ids); v->ids=NULL; v->n=v->cap=0; }

// Result cache — last query & rec_ids
#define CACHE_VERSION 3

typedef enum {
    CACHE_KIND_QUERY = 0,
    CACHE_KIND_TERM  = 1,
    CACHE_KIND_STAGE = 2,
} CacheKind;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t kind;
    uint8_t tag;
    uint64_t generation;
    uint64_t bloom_size;
    uint64_t sig;
    uint32_t count;
    uint32_t data_bytes;
    // followed by delta-encoded ids[data_bytes]
} CacheHeader;

#define CACHE_SIZE_LIMIT (128ULL*1024*1024) // 128 MB
#define CACHE_MAX_ENTRIES 100000000U
#define CACHE_TTL_SECONDS (30ULL*24*60*60) // 30 days
typedef struct {
    uint64_t sig;
    uint64_t last_access;
    uint64_t file_size;
    uint32_t count;
    uint32_t data_bytes;
    uint16_t kind;
    uint16_t tag;
} CacheIndexEntry;

#define CACHE_INDEX_MAGIC   0x43414348u
#define CACHE_INDEX_VERSION 1
#define CACHE_BLOOM_BITS_DEFAULT (1u<<18)
#define CACHE_BLOOM_HASHES 4

typedef struct {
    BOOL loaded;
    BOOL dirty;
    CacheIndexEntry* entries;
    size_t entry_count;
    size_t entry_cap;
    size_t total_bytes;
    uint8_t* bloom;
    size_t bloom_bits;
    size_t bloom_bytes;
    uint32_t bloom_hashes;
    uint64_t generation;
    uint64_t bloom_size;
#ifdef _WIN32
    wchar_t dir[MAX_LONG_PATH];
#else
    char dir[PATH_MAX];
#endif
} CacheIndexState;

static CacheIndexState g_cache_index = {0};
static CRITICAL_SECTION g_cache_index_mu;
static BOOL g_cache_index_mu_init = FALSE;

static wchar_t* path_dirname_w(wchar_t* path){
    wchar_t* a = wcsrchr(path, L'\\');
    wchar_t* b = wcsrchr(path, L'/');
    if(a && b) return (a > b) ? a : b;
    return a ? a : b;
}

#ifndef _WIN32
static char* path_dirname(char* path){
    char* a = strrchr(path, '/');
    return a;
}
#endif

static void cache_index_lock(void){
    if(!g_cache_index_mu_init){
        InitializeCriticalSection(&g_cache_index_mu);
        g_cache_index_mu_init = TRUE;
    }
    EnterCriticalSection(&g_cache_index_mu);
}

static void cache_index_unlock(void){
    if(g_cache_index_mu_init){
        LeaveCriticalSection(&g_cache_index_mu);
    }
}

static uint64_t cache_now_seconds(void){
#ifdef _WIN32
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    return u.QuadPart / 10000000ULL;
#else
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec;
#endif
}

static uint64_t bloom_mix(uint64_t x){
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

static void cache_index_bloom_clear_locked(void){
    if(!g_cache_index.bloom){
        g_cache_index.bloom_bits = CACHE_BLOOM_BITS_DEFAULT;
        g_cache_index.bloom_bytes = g_cache_index.bloom_bits / 8;
        g_cache_index.bloom_hashes = CACHE_BLOOM_HASHES;
        g_cache_index.bloom = (uint8_t*)calloc(g_cache_index.bloom_bytes, 1);
    } else {
        memset(g_cache_index.bloom, 0, g_cache_index.bloom_bytes);
    }
    if(g_cache_index.bloom_hashes == 0) g_cache_index.bloom_hashes = CACHE_BLOOM_HASHES;
    if(g_cache_index.bloom_bits == 0){
        g_cache_index.bloom_bits = CACHE_BLOOM_BITS_DEFAULT;
        g_cache_index.bloom_bytes = g_cache_index.bloom_bits / 8;
        g_cache_index.bloom = (uint8_t*)realloc(g_cache_index.bloom, g_cache_index.bloom_bytes);
        memset(g_cache_index.bloom, 0, g_cache_index.bloom_bytes);
    }
    for(size_t i=0;i<g_cache_index.entry_count;i++){
        CacheIndexEntry* e = &g_cache_index.entries[i];
        uint64_t h1 = e->sig;
        uint64_t h2 = bloom_mix(e->sig ^ ((uint64_t)e->kind<<32) ^ e->tag);
        for(uint32_t j=0;j<g_cache_index.bloom_hashes;j++){
            uint64_t h = h1 + j*h2;
            size_t bit = (size_t)(h % g_cache_index.bloom_bits);
            g_cache_index.bloom[bit>>3] |= (uint8_t)(1u << (bit & 7));
        }
    }
}

static void cache_index_reset_locked(void){
    free(g_cache_index.entries);
    g_cache_index.entries = NULL;
    g_cache_index.entry_count = 0;
    g_cache_index.entry_cap = 0;
    g_cache_index.total_bytes = 0;
    g_cache_index.loaded = FALSE;
    g_cache_index.dirty = FALSE;
    if(g_cache_index.bloom){ memset(g_cache_index.bloom,0,g_cache_index.bloom_bytes); }
}

static void cache_index_reserve_locked(size_t cap){
    if(cap <= g_cache_index.entry_cap) return;
    size_t newcap = g_cache_index.entry_cap ? g_cache_index.entry_cap : 16;
    while(newcap < cap) newcap *= 2;
    CacheIndexEntry* ne = (CacheIndexEntry*)realloc(g_cache_index.entries, newcap * sizeof(CacheIndexEntry));
    if(ne){ g_cache_index.entries = ne; g_cache_index.entry_cap = newcap; }
}

static BOOL cache_index_meta_path(const wchar_t* dbPath,
#ifdef _WIN32
                                 wchar_t* out, size_t outlen
#else
                                 char* out, size_t outlen
#endif
){
#ifdef _WIN32
    wchar_t tmp[MAX_LONG_PATH]; wcscpy_s(tmp, MAX_LONG_PATH, dbPath);
    wchar_t* d = path_dirname_w(tmp);
    if(!d) return FALSE;
    *(d+1) = 0;
    if(swprintf(out, outlen, L"%scache_index.dat", tmp) < 0) return FALSE;
    return TRUE;
#else
    char tmp[PATH_MAX];
    to_utf8(dbPath, tmp, sizeof(tmp));
    char* d = path_dirname(tmp);
    if(!d) return FALSE;
    *(d+1) = 0;
    if(snprintf(out, outlen, "%scache_index.dat", tmp) < 0) return FALSE;
    return TRUE;
#endif
}

static void cache_index_set_dir_locked(const wchar_t* dbPath){
#ifdef _WIN32
    wcscpy_s(g_cache_index.dir, MAX_LONG_PATH, dbPath);
    wchar_t* d = path_dirname_w(g_cache_index.dir);
    if(d) *(d+1)=0; else g_cache_index.dir[0]=0;
#else
    to_utf8(dbPath, g_cache_index.dir, sizeof(g_cache_index.dir));
    char* d = path_dirname(g_cache_index.dir);
    if(d) *(d+1)=0; else g_cache_index.dir[0]=0;
#endif
}

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t bloom_hashes;
    uint32_t bloom_bits;
    uint32_t entry_count;
    uint32_t reserved;
    uint64_t generation;
    uint64_t bloom_size;
} CacheIndexHeader;

static BOOL cache_index_load_locked(const wchar_t* dbPath){
    cache_index_set_dir_locked(dbPath);
#ifdef _WIN32
    wchar_t meta[MAX_LONG_PATH];
    if(!cache_index_meta_path(dbPath, meta, MAX_LONG_PATH)) return FALSE;
    FILE* fp = _wfopen(meta, L"rb");
#else
    char meta[PATH_MAX];
    if(!cache_index_meta_path(dbPath, meta, sizeof(meta))) return FALSE;
    FILE* fp = fopen(meta, "rb");
#endif
    if(!fp){
        g_cache_index.loaded = TRUE;
        g_cache_index.dirty = TRUE;
        g_cache_index.generation = g_db_generation;
        g_cache_index.bloom_size = g_bloom_size;
        cache_index_bloom_clear_locked();
        return TRUE;
    }
    CacheIndexHeader hdr;
    if(fread(&hdr, sizeof(hdr), 1, fp)!=1 || hdr.magic != CACHE_INDEX_MAGIC || hdr.version != CACHE_INDEX_VERSION){
        fclose(fp);
        g_cache_index.loaded = TRUE;
        g_cache_index.dirty = TRUE;
        g_cache_index.generation = g_db_generation;
        g_cache_index.bloom_size = g_bloom_size;
        cache_index_bloom_clear_locked();
        return TRUE;
    }
    if(hdr.generation != g_db_generation || hdr.bloom_size != g_bloom_size){
        fclose(fp);
        g_cache_index.loaded = TRUE;
        g_cache_index.dirty = TRUE;
        g_cache_index.generation = g_db_generation;
        g_cache_index.bloom_size = g_bloom_size;
        cache_index_bloom_clear_locked();
        return TRUE;
    }
    g_cache_index.generation = hdr.generation;
    g_cache_index.bloom_size = hdr.bloom_size;
    g_cache_index.bloom_bits = hdr.bloom_bits ? hdr.bloom_bits : CACHE_BLOOM_BITS_DEFAULT;
    g_cache_index.bloom_bytes = g_cache_index.bloom_bits / 8;
    g_cache_index.bloom_hashes = hdr.bloom_hashes ? hdr.bloom_hashes : CACHE_BLOOM_HASHES;
    g_cache_index.bloom = (uint8_t*)realloc(g_cache_index.bloom, g_cache_index.bloom_bytes);
    if(!g_cache_index.bloom){ fclose(fp); return FALSE; }
    if(fread(g_cache_index.bloom, g_cache_index.bloom_bytes, 1, fp)!=1){ fclose(fp); return FALSE; }
    cache_index_reserve_locked(hdr.entry_count);
    g_cache_index.entry_count = hdr.entry_count;
    if(g_cache_index.entry_count>0){
        if(fread(g_cache_index.entries, sizeof(CacheIndexEntry), g_cache_index.entry_count, fp)!=g_cache_index.entry_count){ fclose(fp); return FALSE; }
    }
    fclose(fp);
    g_cache_index.total_bytes = 0;
    for(size_t i=0;i<g_cache_index.entry_count;i++) g_cache_index.total_bytes += g_cache_index.entries[i].file_size;
    g_cache_index.loaded = TRUE;
    g_cache_index.dirty = FALSE;
    return TRUE;
}

static BOOL cache_index_flush_locked(void){
    if(!g_cache_index.dirty) return TRUE;
    if(!g_cache_index.dir[0]) return FALSE;
#ifdef _WIN32
    wchar_t meta[MAX_LONG_PATH];
    if(swprintf(meta, MAX_LONG_PATH, L"%scache_index.dat", g_cache_index.dir) < 0) return FALSE;
    wchar_t tmp[MAX_LONG_PATH];
    if(swprintf(tmp, MAX_LONG_PATH, L"%scache_index.tmp", g_cache_index.dir) < 0) return FALSE;
    FILE* fp = _wfopen(tmp, L"wb");
#else
    char meta[PATH_MAX];
    snprintf(meta, sizeof(meta), "%scache_index.dat", g_cache_index.dir);
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%scache_index.tmp", g_cache_index.dir);
    FILE* fp = fopen(tmp, "wb");
#endif
    if(!fp) return FALSE;
    CacheIndexHeader hdr = {
        .magic = CACHE_INDEX_MAGIC,
        .version = CACHE_INDEX_VERSION,
        .bloom_hashes = (uint16_t)g_cache_index.bloom_hashes,
        .bloom_bits = (uint32_t)g_cache_index.bloom_bits,
        .entry_count = (uint32_t)g_cache_index.entry_count,
        .reserved = 0,
        .generation = g_cache_index.generation,
        .bloom_size = g_cache_index.bloom_size,
    };
    fwrite(&hdr, sizeof(hdr), 1, fp);
    fwrite(g_cache_index.bloom, g_cache_index.bloom_bytes, 1, fp);
    if(g_cache_index.entry_count>0){
        fwrite(g_cache_index.entries, sizeof(CacheIndexEntry), g_cache_index.entry_count, fp);
    }
    fclose(fp);
#ifdef _WIN32
    _wremove(meta);
    _wrename(tmp, meta);
#else
    remove(meta);
    rename(tmp, meta);
#endif
    g_cache_index.dirty = FALSE;
    return TRUE;
}

static ptrdiff_t cache_index_find_locked(uint64_t sig, uint16_t kind, uint16_t tag){
    for(size_t i=0;i<g_cache_index.entry_count;i++){
        CacheIndexEntry* e = &g_cache_index.entries[i];
        if(e->sig==sig && e->kind==kind && e->tag==tag) return (ptrdiff_t)i;
    }
    return -1;
}

static BOOL cache_index_bloom_maybe_has_locked(uint64_t sig, uint16_t kind, uint16_t tag){
    if(!g_cache_index.bloom || g_cache_index.bloom_bits==0) return TRUE;
    uint64_t h1 = sig;
    uint64_t h2 = bloom_mix(sig ^ ((uint64_t)kind<<32) ^ tag);
    for(uint32_t j=0;j<g_cache_index.bloom_hashes;j++){
        uint64_t h = h1 + j*h2;
        size_t bit = (size_t)(h % g_cache_index.bloom_bits);
        if(!(g_cache_index.bloom[bit>>3] & (uint8_t)(1u << (bit & 7)))) return FALSE;
    }
    return TRUE;
}

static void cache_index_bloom_add_locked(uint64_t sig, uint16_t kind, uint16_t tag){
    if(!g_cache_index.bloom || g_cache_index.bloom_bits==0) return;
    uint64_t h1 = sig;
    uint64_t h2 = bloom_mix(sig ^ ((uint64_t)kind<<32) ^ tag);
    for(uint32_t j=0;j<g_cache_index.bloom_hashes;j++){
        uint64_t h = h1 + j*h2;
        size_t bit = (size_t)(h % g_cache_index.bloom_bits);
        g_cache_index.bloom[bit>>3] |= (uint8_t)(1u << (bit & 7));
    }
}

static BOOL cache_index_delete_entry_locked(size_t idx){
    if(idx >= g_cache_index.entry_count) return FALSE;
    CacheIndexEntry e = g_cache_index.entries[idx];
#ifdef _WIN32
    if(g_cache_index.dir[0]){
        wchar_t path[MAX_LONG_PATH];
        if(e.kind == CACHE_KIND_QUERY){
            swprintf(path, MAX_LONG_PATH, L"%scache_%016llx.tmp", g_cache_index.dir, (unsigned long long)e.sig);
        } else if(e.kind == CACHE_KIND_TERM){
            swprintf(path, MAX_LONG_PATH, L"%scache_term_%016llx.tmp", g_cache_index.dir, (unsigned long long)e.sig);
        } else {
            swprintf(path, MAX_LONG_PATH, L"%scache_stage%u_%016llx.tmp", g_cache_index.dir, e.tag, (unsigned long long)e.sig);
        }
        _wremove(path);
    }
#else
    if(g_cache_index.dir[0]){
        char path[PATH_MAX];
        if(e.kind == CACHE_KIND_QUERY){
            snprintf(path, sizeof(path), "%scache_%016llx.tmp", g_cache_index.dir, (unsigned long long)e.sig);
        } else if(e.kind == CACHE_KIND_TERM){
            snprintf(path, sizeof(path), "%scache_term_%016llx.tmp", g_cache_index.dir, (unsigned long long)e.sig);
        } else {
            snprintf(path, sizeof(path), "%scache_stage%u_%016llx.tmp", g_cache_index.dir, e.tag, (unsigned long long)e.sig);
        }
        remove(path);
    }
#endif
    if(idx+1 < g_cache_index.entry_count){
        memmove(&g_cache_index.entries[idx], &g_cache_index.entries[idx+1], (g_cache_index.entry_count-idx-1)*sizeof(CacheIndexEntry));
    }
    g_cache_index.entry_count--;
    g_cache_index.total_bytes = (g_cache_index.total_bytes >= e.file_size) ? g_cache_index.total_bytes - e.file_size : 0;
    cache_index_bloom_clear_locked();
    g_cache_index.dirty = TRUE;
    return TRUE;
}

static void cache_index_evict_locked(void){
    uint64_t now = cache_now_seconds();
    for(size_t i=0;i<g_cache_index.entry_count;){
        CacheIndexEntry* e = &g_cache_index.entries[i];
        if(now > e->last_access && now - e->last_access > CACHE_TTL_SECONDS){
            cache_index_delete_entry_locked(i);
            continue;
        }
        i++;
    }
    while(g_cache_index.total_bytes > CACHE_SIZE_LIMIT && g_cache_index.entry_count>0){
        size_t oldest = 0;
        uint64_t best = g_cache_index.entries[0].last_access;
        for(size_t i=1;i<g_cache_index.entry_count;i++){
            if(g_cache_index.entries[i].last_access < best){ best = g_cache_index.entries[i].last_access; oldest = i; }
        }
        cache_index_delete_entry_locked(oldest);
    }
}

static BOOL cache_index_prepare(const wchar_t* dbPath){
    cache_index_lock();
    BOOL ok = TRUE;
    if(!g_cache_index.loaded || g_cache_index.generation != g_db_generation || g_cache_index.bloom_size != g_bloom_size){
        cache_index_reset_locked();
        ok = cache_index_load_locked(dbPath);
        if(ok) cache_index_bloom_clear_locked();
    }
    if(ok) cache_index_evict_locked();
    if(ok) cache_index_flush_locked();
    cache_index_unlock();
    return ok;
}

static BOOL cache_index_upsert(uint64_t sig, uint16_t kind, uint16_t tag, uint32_t count, uint32_t data_bytes, uint64_t file_size){
    cache_index_lock();
    if(!g_cache_index.loaded){
        cache_index_unlock();
        return FALSE;
    }
    ptrdiff_t idx = cache_index_find_locked(sig, kind, tag);
    if(idx < 0){
        cache_index_reserve_locked(g_cache_index.entry_count+1);
        idx = (ptrdiff_t)g_cache_index.entry_count++;
    }
    CacheIndexEntry* e = &g_cache_index.entries[idx];
    e->sig = sig;
    e->kind = kind;
    e->tag = tag;
    e->count = count;
    e->data_bytes = data_bytes;
    g_cache_index.total_bytes -= (g_cache_index.total_bytes >= e->file_size) ? e->file_size : 0;
    e->file_size = file_size;
    e->last_access = cache_now_seconds();
    g_cache_index.total_bytes += e->file_size;
    g_cache_index.dirty = TRUE;
    cache_index_bloom_add_locked(sig, kind, tag);
    cache_index_evict_locked();
    cache_index_flush_locked();
    cache_index_unlock();
    return TRUE;
}

static void cache_index_remove(uint64_t sig, uint16_t kind, uint16_t tag){
    cache_index_lock();
    ptrdiff_t idx = cache_index_find_locked(sig, kind, tag);
    if(idx >= 0){
        cache_index_delete_entry_locked((size_t)idx);
        cache_index_flush_locked();
    }
    cache_index_unlock();
}

#ifdef _WIN32
static BOOL cache_build_path(CacheKind kind, uint16_t tag, uint64_t sig, wchar_t* out, size_t outlen){
    if(!g_cache_index.dir[0]) return FALSE;
    switch(kind){
        case CACHE_KIND_QUERY:
            return swprintf(out, outlen, L"%scache_%016llx.tmp", g_cache_index.dir, (unsigned long long)sig) >= 0;
        case CACHE_KIND_TERM:
            return swprintf(out, outlen, L"%scache_term_%016llx.tmp", g_cache_index.dir, (unsigned long long)sig) >= 0;
        case CACHE_KIND_STAGE:
            return swprintf(out, outlen, L"%scache_stage%u_%016llx.tmp", g_cache_index.dir, tag, (unsigned long long)sig) >= 0;
    }
    return FALSE;
}
#else
static BOOL cache_build_path(CacheKind kind, uint16_t tag, uint64_t sig, char* out, size_t outlen){
    if(!g_cache_index.dir[0]) return FALSE;
    switch(kind){
        case CACHE_KIND_QUERY:
            return snprintf(out, outlen, "%scache_%016llx.tmp", g_cache_index.dir, (unsigned long long)sig) >= 0;
        case CACHE_KIND_TERM:
            return snprintf(out, outlen, "%scache_term_%016llx.tmp", g_cache_index.dir, (unsigned long long)sig) >= 0;
        case CACHE_KIND_STAGE:
            return snprintf(out, outlen, "%scache_stage%u_%016llx.tmp", g_cache_index.dir, tag, (unsigned long long)sig) >= 0;
    }
    return FALSE;
}
#endif

static size_t varint_length(uint64_t v){
    size_t len = 1;
    while(v >= 0x80){ v >>= 7; len++; }
    return len;
}

static uint8_t* varint_write(uint8_t* p, uint64_t v){
    while(v >= 0x80){ *p++ = (uint8_t)((v & 0x7F) | 0x80); v >>= 7; }
    *p++ = (uint8_t)(v & 0x7F);
    return p;
}

static BOOL varint_read(const uint8_t** p, const uint8_t* end, uint64_t* out){
    uint64_t value = 0; int shift = 0;
    const uint8_t* cur = *p;
    while(cur < end && shift <= 63){
        uint8_t byte = *cur++;
        value |= (uint64_t)(byte & 0x7F) << shift;
        if(!(byte & 0x80)){ *p = cur; *out = value; return TRUE; }
        shift += 7;
    }
    return FALSE;
}

static size_t encode_ids_delta(const uint64_t* ids, size_t n, uint8_t** out_buf){
    if(n==0){ *out_buf=NULL; return 0; }
    size_t total = 0;
    uint64_t prev = 0;
    for(size_t i=0;i<n;i++){
        uint64_t delta = (i==0) ? ids[i] : (ids[i] - prev);
        total += varint_length(delta);
        prev = ids[i];
    }
    uint8_t* buf = (uint8_t*)malloc(total);
    if(!buf){ *out_buf=NULL; return 0; }
    uint8_t* ptr = buf;
    prev = 0;
    for(size_t i=0;i<n;i++){
        uint64_t delta = (i==0) ? ids[i] : (ids[i] - prev);
        ptr = varint_write(ptr, delta);
        prev = ids[i];
    }
    *out_buf = buf;
    return total;
}

static BOOL decode_ids_delta(const uint8_t* data, size_t len, size_t count, IdVec* out){
    const uint8_t* ptr = data;
    const uint8_t* end = data + len;
    uint64_t prev = 0;
    idvec_reserve(out, count);
    for(size_t i=0;i<count;i++){
        uint64_t delta;
        if(!varint_read(&ptr, end, &delta)) return FALSE;
        uint64_t value = (i==0) ? delta : prev + delta;
        idvec_push(out, value);
        prev = value;
    }
    return ptr == end;
}


static BOOL try_load_cache_internal(const wchar_t* dbPath, const char* key, CacheKind kind, uint16_t tag, IdVec* out){
    if(!key || !out) return FALSE;
    uint64_t sig = hash64(key, strlen(key));
    if(!cache_index_prepare(dbPath)) return FALSE;
    cache_index_lock();
    BOOL maybe = cache_index_bloom_maybe_has_locked(sig, kind, tag);
    cache_index_unlock();
    if(!maybe) return FALSE;
#ifdef _WIN32
    wchar_t path[MAX_LONG_PATH];
    if(!cache_build_path(kind, tag, sig, path, MAX_LONG_PATH)) return FALSE;
    wchar_t lp[MAX_LONG_PATH]; make_long_path(path, lp, MAX_LONG_PATH);
    HANDLE f = CreateFileW(lp, GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if(f==INVALID_HANDLE_VALUE){ cache_index_remove(sig, kind, tag); return FALSE; }
    DWORD sz_raw = GetFileSize(f, NULL);
    if(sz_raw < sizeof(CacheHeader)){ CloseHandle(f); cache_index_remove(sig, kind, tag); return FALSE; }
    size_t file_size = (size_t)sz_raw;
    uint8_t* buf = (uint8_t*)malloc(file_size);
    if(!buf){ CloseHandle(f); return FALSE; }
    DWORD read = 0;
    BOOL okread = ReadFile(f, buf, (DWORD)file_size, &read, NULL);
    CloseHandle(f);
    if(!okread || read != file_size){ free(buf); cache_index_remove(sig, kind, tag); return FALSE; }
#else
    char path[PATH_MAX];
    if(!cache_build_path(kind, tag, sig, path, sizeof(path))) return FALSE;
    int fd = open(path, O_RDONLY);
    if(fd < 0){ cache_index_remove(sig, kind, tag); return FALSE; }
    struct stat st;
    if(fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(CacheHeader)){ close(fd); cache_index_remove(sig, kind, tag); return FALSE; }
    size_t file_size = (size_t)st.st_size;
    uint8_t* buf = (uint8_t*)malloc(file_size);
    if(!buf){ close(fd); return FALSE; }
    ssize_t rd = read(fd, buf, file_size);
    close(fd);
    if(rd != (ssize_t)file_size){ free(buf); cache_index_remove(sig, kind, tag); return FALSE; }
#endif
    CacheHeader header = *(CacheHeader*)buf;
    const uint8_t* data = buf + sizeof(CacheHeader);
    BOOL ok = FALSE;
    size_t expected = sizeof(CacheHeader) + header.data_bytes;
    if(header.magic == CACHE_MAGIC &&
       header.version == CACHE_VERSION &&
       header.kind == kind &&
       header.tag == tag &&
       header.sig == sig &&
       header.generation == g_db_generation &&
       header.bloom_size == g_bloom_size &&
       header.count <= CACHE_MAX_ENTRIES &&
       expected == file_size){
        idvec_free(out); idvec_init(out);
        if(header.count==0 || decode_ids_delta(data, header.data_bytes, header.count, out)){
            out->n = header.count;
            if(out->cap < out->n) out->cap = out->n;
            ok = TRUE;
        }
    }
    free(buf);
    if(ok){
        cache_index_upsert(sig, kind, tag, header.count, header.data_bytes, sizeof(CacheHeader)+header.data_bytes);
        return TRUE;
    }
    cache_index_remove(sig, kind, tag);
    return FALSE;
}

static BOOL try_load_cache(const wchar_t* dbPath, const char* qstr, IdVec* out){
    return try_load_cache_internal(dbPath, qstr, CACHE_KIND_QUERY, 0, out);
}
static BOOL save_cache_internal(const wchar_t* dbPath, const char* key, CacheKind kind, uint16_t tag, const IdVec* ids){
    if(!key || !ids) return FALSE;
    uint64_t sig = hash64(key, strlen(key));
    if(!cache_index_prepare(dbPath)) return FALSE;
    uint8_t* encoded = NULL;
    size_t data_bytes = encode_ids_delta(ids->ids, ids->n, &encoded);
    size_t total = sizeof(CacheHeader) + data_bytes;
#ifdef _WIN32
    wchar_t path[MAX_LONG_PATH];
    if(!cache_build_path(kind, tag, sig, path, MAX_LONG_PATH)){ free(encoded); return FALSE; }
    wchar_t lp[MAX_LONG_PATH]; make_long_path(path, lp, MAX_LONG_PATH);
    HANDLE f = CreateFileW(lp, GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if(f==INVALID_HANDLE_VALUE){ free(encoded); return FALSE; }
    CacheHeader header = { .magic=CACHE_MAGIC, .version=CACHE_VERSION, .kind=(uint8_t)kind, .tag=(uint8_t)tag, .generation=g_db_generation, .bloom_size=g_bloom_size, .sig=sig, .count=(uint32_t)ids->n, .data_bytes=(uint32_t)data_bytes };
    DWORD written = 0;
    if(!WriteFile(f, &header, sizeof(header), &written, NULL) || written != sizeof(header)){
        CloseHandle(f); free(encoded); cache_index_remove(sig, kind, tag); return FALSE;
    }
    if(data_bytes>0){
        if(!WriteFile(f, encoded, (DWORD)data_bytes, &written, NULL) || written != data_bytes){
            CloseHandle(f); free(encoded); cache_index_remove(sig, kind, tag); return FALSE;
        }
    }
    FlushFileBuffers(f);
    CloseHandle(f);
#else
    char path[PATH_MAX];
    if(!cache_build_path(kind, tag, sig, path, sizeof(path))){ free(encoded); return FALSE; }
    int fd = open(path, O_WRONLY|O_CREAT|O_TRUNC, 0666);
    if(fd < 0){ free(encoded); return FALSE; }
    CacheHeader header = { .magic=CACHE_MAGIC, .version=CACHE_VERSION, .kind=(uint8_t)kind, .tag=(uint8_t)tag, .generation=g_db_generation, .bloom_size=g_bloom_size, .sig=sig, .count=(uint32_t)ids->n, .data_bytes=(uint32_t)data_bytes };
    if(write(fd, &header, sizeof(header)) != (ssize_t)sizeof(header) || (data_bytes>0 && write(fd, encoded, data_bytes) != (ssize_t)data_bytes)){
        close(fd); free(encoded); cache_index_remove(sig, kind, tag); return FALSE;
    }
    fsync(fd);
    close(fd);
#endif
    free(encoded);
    cache_index_upsert(sig, kind, tag, (uint32_t)ids->n, (uint32_t)data_bytes, total);
    return TRUE;
}

static void save_cache(const wchar_t* dbPath, const char* qstr, const IdVec* ids){
    save_cache_internal(dbPath, qstr, CACHE_KIND_QUERY, 0, ids);
}

// Per-term cache helpers
static char term_prefix(TermType t){
    switch(t){
        case TERM_NAME: return 'N';
        case TERM_AUTHOR: return 'A';
        case TERM_CAMERA: return 'M';
        case TERM_LENS: return 'L';
        case TERM_ARTIST: return 'R';
        case TERM_ALBUM: return 'B';
        case TERM_TITLE: return 'T';
        case TERM_EXT: return 'E';
        case TERM_CONTENT: return 'C';
    }
    return 'X';
}

static BOOL try_load_term_cache(TermType ttype, const char* term, IdVec* out){
    if(!g_db_path[0]) return FALSE;
    char key[512];
    key[0] = term_prefix(ttype);
    strncpy(key+1, term, sizeof(key)-2);
    key[sizeof(key)-1] = 0;
    return try_load_cache_internal(g_db_path, key, CACHE_KIND_TERM, (uint16_t)ttype, out);
}

static void save_term_cache(TermType ttype, const char* term, const IdVec* ids){
    if(!g_db_path[0]) return;
    char key[512];
    key[0] = term_prefix(ttype);
    strncpy(key+1, term, sizeof(key)-2);
    key[sizeof(key)-1] = 0;
    save_cache_internal(g_db_path, key, CACHE_KIND_TERM, (uint16_t)ttype, ids);
}

// Intersect postings

static int cmp_u64(const void* A, const void* B){
    uint64_t a = *(const uint64_t*)A, b = *(const uint64_t*)B;
    return (a>b) - (a<b);
}
static void sort_unique(IdVec* v){
    qsort(v->ids, v->n, sizeof(uint64_t), cmp_u64);
    size_t w=0; uint64_t prev=0;
    for(size_t i=0;i<v->n;i++){
        if(i==0 || v->ids[i]!=prev){
            v->ids[w++]=prev=v->ids[i];
        }
    }
    v->n = w;
}

typedef struct {
    MDB_env* envs[4];
    SRWLOCK  lock;
    int count;
} EnvPool;

static void envpool_init(EnvPool* p, const char* dbpath, int want){
    InitializeSRWLock(&p->lock); p->count = (want<1)?1:(want>4?4:want);
    for(int i=0;i<p->count;i++){
        mdb_env_create(&p->envs[i]); mdb_env_set_maxdbs(p->envs[i],64);
        mdb_env_open(p->envs[i], dbpath, MDB_RDONLY, 0664);
    }
}

static void envpool_close(EnvPool* p){
    for(int i=0;i<p->count;i++){ if(p->envs[i]) mdb_env_close(p->envs[i]); }
}

typedef struct {
    uint64_t rec_id;
    float score;
} RankedResult;

typedef struct FilterArgs {
    uint64_t* ids;
    size_t start, end;
    SearchQuery* q;
    RankedResult* out;
    size_t outn;
    size_t outcap;
    MDB_env* env;
    MDB_dbi dbi_strings;
    MDB_dbi dbi_records;
    size_t total_docs;
    size_t docs_with_term;
} FilterArgs;

#define FILTER_TXN_RENEW_BATCH 256

#ifdef _WIN32
static INIT_ONCE g_filter_txn_sem_once = INIT_ONCE_STATIC_INIT;
static HANDLE g_filter_txn_sem = NULL;

static BOOL CALLBACK filter_txn_sem_init_once(PINIT_ONCE init_once, PVOID param, PVOID* context){
    (void)init_once; (void)param; (void)context;
    int max = g_config.max_search_workers;
    if(max < 1) max = 1;
    g_filter_txn_sem = CreateSemaphoreW(NULL, max, max, NULL);
    return g_filter_txn_sem != NULL;
}

static void filter_txn_sem_acquire(void){
    InitOnceExecuteOnce(&g_filter_txn_sem_once, filter_txn_sem_init_once, NULL, NULL);
    if(g_filter_txn_sem){
        WaitForSingleObject(g_filter_txn_sem, INFINITE);
    }
}

static void filter_txn_sem_release(void){
    if(g_filter_txn_sem){
        ReleaseSemaphore(g_filter_txn_sem, 1, NULL);
    }
}
#else
static pthread_once_t g_filter_txn_sem_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_filter_txn_sem_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_filter_txn_sem_cond = PTHREAD_COND_INITIALIZER;
static int g_filter_txn_sem_count = 0;
static int g_filter_txn_sem_max = 0;

static void filter_txn_sem_init(void){
    g_filter_txn_sem_count = g_config.max_search_workers;
    if(g_filter_txn_sem_count < 1) g_filter_txn_sem_count = 1;
    g_filter_txn_sem_max = g_filter_txn_sem_count;
}

static void filter_txn_sem_acquire(void){
    pthread_once(&g_filter_txn_sem_once, filter_txn_sem_init);
    pthread_mutex_lock(&g_filter_txn_sem_mutex);
    while(g_filter_txn_sem_count == 0){
        pthread_cond_wait(&g_filter_txn_sem_cond, &g_filter_txn_sem_mutex);
    }
    g_filter_txn_sem_count--;
    pthread_mutex_unlock(&g_filter_txn_sem_mutex);
}

static void filter_txn_sem_release(void){
    pthread_mutex_lock(&g_filter_txn_sem_mutex);
    if(g_filter_txn_sem_count < g_filter_txn_sem_max){
        g_filter_txn_sem_count++;
        pthread_cond_signal(&g_filter_txn_sem_cond);
    }
    pthread_mutex_unlock(&g_filter_txn_sem_mutex);
}
#endif

static float calculate_relevance(MDB_txn* txn, MDB_dbi dbi_strings, const DbRecord* r, const char* parent_utf8, const char* name_utf8, const SearchQuery* q, size_t total_docs, size_t docs_with_term);

static DWORD WINAPI filter_worker_thread(void* p){
    FilterArgs* a=(FilterArgs*)p;
    MDB_txn* txn=NULL;
    MDB_dbi dbi_strings = a->dbi_strings;
    MDB_dbi dbi_records = a->dbi_records;
    filter_txn_sem_acquire();
    if(mdb_txn_begin(a->env,NULL,MDB_RDONLY,&txn)!=0){
        filter_txn_sem_release();
        return 0;
    }
    size_t processed = 0;
    size_t path_filter_len = a->q->path_filter ? strlen(a->q->path_filter) : 0;
    for(size_t i=a->start;i<a->end;i++){
        if(processed && (processed % FILTER_TXN_RENEW_BATCH)==0){
            mdb_txn_reset(txn);
            if(mdb_txn_renew(txn)!=0){
                goto done;
            }
        }
        processed++;
        uint64_t rid = a->ids[i];
        MDB_val rk={.mv_data=&rid,.mv_size=sizeof(rid)}, rv;
        if(mdb_get(txn, dbi_records, &rk, &rv)!=0 || rv.mv_size<sizeof(DbRecord)) continue;
        DbRecord* r = (DbRecord*)rv.mv_data;
        if(r->file_size < a->q->size_min || r->file_size > a->q->size_max) continue;
        uint64_t day = filetime_days(r->modified_time);
        if(day < a->q->date_min_day || day > a->q->date_max_day) continue;
        MDB_val pk={.mv_data=&r->parent_str_id,.mv_size=sizeof(r->parent_str_id)}, pv;
        MDB_val nk={.mv_data=&r->name_str_id,.mv_size=sizeof(r->name_str_id)}, nv;
        char* parent = NULL;
        char* name = NULL;
        size_t parent_needed = 0;
        db_parent_cache_copy(r->parent_str_id, NULL, 0, &parent_needed);
        if(parent_needed > 0){
            parent = (char*)_malloca(parent_needed);
            if(parent && !db_parent_cache_copy(r->parent_str_id, parent, parent_needed, NULL)){
                _freea(parent);
                parent = NULL;
            }
        }
        if(!parent){
            MDB_val parent_val;
            if(mdb_get(txn, dbi_strings, &pk, &pv)!=0) goto next_record;
            string_value_parse(&pv, &parent_val, NULL);
            parent = (char*)_malloca(parent_val.mv_size + 1);
            if(!parent) goto next_record;
            memcpy(parent, parent_val.mv_data, parent_val.mv_size);
            parent[parent_val.mv_size] = 0;
            db_parent_cache_put(r->parent_str_id, parent, parent_val.mv_size);
        }
        if(mdb_get(txn, dbi_strings, &nk, &nv)!=0) goto next_record;
        MDB_val name_val; StringMeta name_meta;
        string_value_parse(&nv, &name_val, &name_meta);
        name = (char*)_malloca(name_val.mv_size + 1);
        if(!name) goto next_record;
        memcpy(name, name_val.mv_data, name_val.mv_size);
        name[name_val.mv_size] = 0;
        if(a->q->path_filter){ if(strncmp(parent, a->q->path_filter, path_filter_len)!=0) goto next_record; }
        if(a->q->name_pattern){
            char norm[512];
            normalize_filename_utf8(name, norm, sizeof(norm));
            char* pat=_strdup(a->q->name_pattern); lowercase_ascii(pat,strlen(pat));
            for(size_t j=0;pat[j];++j){ if(pat[j]=='_'||pat[j]=='-') pat[j]=' '; }
            int maxd = (int)((strlen(pat)+4)/5);
            BOOL ok = fuzzy_match(norm, pat, maxd);
            free(pat);
            if(!ok) goto next_record;
        }
        float score = calculate_relevance(txn, dbi_strings, r, parent, name, a->q, a->total_docs, a->docs_with_term);
        if(a->outn < a->outcap){
            a->out[a->outn].rec_id = rid;
            a->out[a->outn].score = score;
            a->outn++;
        }
next_record:
        if(name){ _freea(name); name = NULL; }
        if(parent){ _freea(parent); parent = NULL; }
    }
done:
    if(txn) mdb_txn_abort(txn);
    filter_txn_sem_release();
    return 0;
}


static int cmp_rank(const void* A, const void* B){
    float a = ((const RankedResult*)A)->score;
    float b = ((const RankedResult*)B)->score;
    return (a<b) - (a>b);
}
static BOOL name_exact_prefix(const char* name, const char* pat, BOOL* exact, BOOL* prefix){
    size_t nl=strlen(name), pl=strlen(pat);
    *exact = (nl==pl && _stricmp(name, pat)==0);
    *prefix = (nl>=pl && _strnicmp(name, pat, pl)==0);
    return TRUE;
}

static int count_term_occurrences(const char* text, const char* term){
    int c=0; size_t tlen=strlen(term); const char* p=text;
    while((p = StrStrIA(p, term))!=NULL){ c++; p+=tlen; }
    return c;
}

static float calculate_relevance(MDB_txn* txn, MDB_dbi dbi_strings, const DbRecord* r, const char* parent_utf8, const char* name_utf8, const SearchQuery* q, size_t total_docs, size_t docs_with_term){
    const float W_FILENAME=0.4f, W_CONTENT=0.4f, W_METADATA=0.1f, W_RECENCY=0.1f;
    float fname_score=0.0f, content_score=0.0f, meta_score=0.0f, recency_score=0.0f;
    (void)parent_utf8;
    if(q->name_pattern){
        int tf = count_term_occurrences(name_utf8, q->name_pattern);
        int dl = (int)strlen(name_utf8);
        const float avgdl = 16.0f;
        fname_score = bm25_score(tf, dl, avgdl, (int)total_docs, (int)docs_with_term);
        BOOL ex=FALSE, pr=FALSE; name_exact_prefix(name_utf8, q->name_pattern, &ex, &pr);
        if(ex) fname_score += 2.0f; else if(pr) fname_score += 1.0f;
    }
    if(q->content_pattern && r->content_str_id){
        MDB_val ck={.mv_data=&r->content_str_id,.mv_size=sizeof(r->content_str_id)}, cv;
        if(mdb_get(txn, dbi_strings, &ck, &cv)==0){
            MDB_val text_val; StringMeta meta;
            string_value_parse(&cv, &text_val, &meta);
            char* content = (char*)_malloca(text_val.mv_size + 1);
            if(content){
                memcpy(content, text_val.mv_data, text_val.mv_size);
                content[text_val.mv_size] = 0;
                int tf = count_term_occurrences(content, q->content_pattern);
                int dl = (int)text_val.mv_size;
                const float avgdl = 1000.0f;
                content_score = bm25_score(tf, dl, avgdl, (int)total_docs, (int)docs_with_term);
                _freea(content);
            }
        }
    }
    if(q->author_pattern && r->author_str_id){
        MDB_val ak={.mv_data=&r->author_str_id,.mv_size=sizeof(r->author_str_id)}, av;
        if(mdb_get(txn, dbi_strings, &ak, &av)==0){
            MDB_val text_val; StringMeta meta;
            string_value_parse(&av, &text_val, &meta);
            char* author=(char*)_malloca(text_val.mv_size + 1);
            if(author){
                memcpy(author, text_val.mv_data, text_val.mv_size);
                author[text_val.mv_size] = 0;
                if(_stricmp(author, q->author_pattern)==0) meta_score += 1.0f;
                _freea(author);
            }
        }
    }
    if(q->camera_pattern && r->camera_str_id){
        MDB_val ck={.mv_data=&r->camera_str_id,.mv_size=sizeof(r->camera_str_id)}, cv;
        if(mdb_get(txn, dbi_strings, &ck, &cv)==0){
            MDB_val text_val; StringMeta meta;
            string_value_parse(&cv, &text_val, &meta);
            char* camera=(char*)_malloca(text_val.mv_size + 1);
            if(camera){
                memcpy(camera, text_val.mv_data, text_val.mv_size);
                camera[text_val.mv_size] = 0;
                if(_stricmp(camera, q->camera_pattern)==0) meta_score += 1.0f;
                _freea(camera);
            }
        }
    }
    if(q->lens_pattern && r->lens_str_id){
        MDB_val lk={.mv_data=&r->lens_str_id,.mv_size=sizeof(r->lens_str_id)}, lv;
        if(mdb_get(txn, dbi_strings, &lk, &lv)==0){
            MDB_val text_val; StringMeta meta;
            string_value_parse(&lv, &text_val, &meta);
            char* lens=(char*)_malloca(text_val.mv_size + 1);
            if(lens){
                memcpy(lens, text_val.mv_data, text_val.mv_size);
                lens[text_val.mv_size] = 0;
                if(_stricmp(lens, q->lens_pattern)==0) meta_score += 1.0f;
                _freea(lens);
            }
        }
    }
    if(q->artist_pattern && r->artist_str_id){
        MDB_val ark={.mv_data=&r->artist_str_id,.mv_size=sizeof(r->artist_str_id)}, av2;
        if(mdb_get(txn, dbi_strings, &ark, &av2)==0){
            MDB_val text_val; StringMeta meta;
            string_value_parse(&av2, &text_val, &meta);
            char* artist=(char*)_malloca(text_val.mv_size + 1);
            if(artist){
                memcpy(artist, text_val.mv_data, text_val.mv_size);
                artist[text_val.mv_size] = 0;
                if(_stricmp(artist, q->artist_pattern)==0) meta_score += 1.0f;
                _freea(artist);
            }
        }
    }
    if(q->album_pattern && r->album_str_id){
        MDB_val abk={.mv_data=&r->album_str_id,.mv_size=sizeof(r->album_str_id)}, abv;
        if(mdb_get(txn, dbi_strings, &abk, &abv)==0){
            MDB_val text_val; StringMeta meta;
            string_value_parse(&abv, &text_val, &meta);
            char* album=(char*)_malloca(text_val.mv_size + 1);
            if(album){
                memcpy(album, text_val.mv_data, text_val.mv_size);
                album[text_val.mv_size] = 0;
                if(_stricmp(album, q->album_pattern)==0) meta_score += 1.0f;
                _freea(album);
            }
        }
    }
    if(q->title_pattern && r->title_str_id){
        MDB_val tk={.mv_data=&r->title_str_id,.mv_size=sizeof(r->title_str_id)}, tv;
        if(mdb_get(txn, dbi_strings, &tk, &tv)==0){
            MDB_val text_val; StringMeta meta;
            string_value_parse(&tv, &text_val, &meta);
            char* title=(char*)_malloca(text_val.mv_size + 1);
            if(title){
                memcpy(title, text_val.mv_data, text_val.mv_size);
                title[text_val.mv_size] = 0;
                if(_stricmp(title, q->title_pattern)==0) meta_score += 1.0f;
                _freea(title);
            }
        }
    }
    if(q->ext_pattern){
        const char* ext = PathFindExtensionA(name_utf8);
        if(ext && ext[0]){ ext++; if(_stricmp(ext, q->ext_pattern)==0) meta_score += 1.0f; }
    }
    uint64_t days_old = today_day() - filetime_days(r->modified_time);
    if(days_old < 7) recency_score = 1.0f; else if(days_old < 30) recency_score = 0.5f;
    float final = fname_score*W_FILENAME + content_score*W_CONTENT + meta_score*W_METADATA + recency_score*W_RECENCY;
    return final;
}
static size_t binary_search_ge(const uint64_t* arr, size_t n, uint64_t x){
    size_t lo=0, hi=n;
    while(lo<hi){ size_t mid=lo+((hi-lo)>>1); if(arr[mid]<x) lo=mid+1; else hi=mid; }
    return lo;
}

static void traditional_intersect(IdVec* a, const IdVec* b){
    if(a->n==0 || b->n==0){ a->n=0; return; }
    size_t i=0,j=0,w=0;
    while(i<a->n && j<b->n){
        uint64_t x=a->ids[i], y=b->ids[j];
        if(x==y){ a->ids[w++]=x; i++; j++; }
        else if(x<y) i++;
        else j++;
    }
    a->n=w;
}

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386) || defined(_M_IX86)
#include <immintrin.h>
#ifdef _MSC_VER
#include <intrin.h>
#endif
static int cpu_has_avx2(void){
#if defined(__GNUC__)
    return __builtin_cpu_supports("avx2");
#elif defined(_MSC_VER)
    int info[4]; __cpuid(info, 0); if(info[0] >= 7){ __cpuidex(info,7,0); return (info[1] & (1<<5)) != 0; } return 0;
#else
    return 0;
#endif
}
static void intersect_avx2(IdVec* a, const IdVec* b){
#if defined(__AVX2__)
    size_t i=0,j=0,w=0;
    while(i<a->n && j<b->n){
        if(j+4<=b->n){
            __m256i bx=_mm256_loadu_si256((const __m256i*)(b->ids+j));
            __m256i xx=_mm256_set1_epi64x(a->ids[i]);
            __m256i cmp=_mm256_cmpeq_epi64(xx,bx);
            int mask=_mm256_movemask_pd(_mm256_castsi256_pd(cmp));
            if(mask){
                int lane=__builtin_ctz(mask);
                a->ids[w++]=a->ids[i++];
                j += lane+1;
                continue;
            }
            if(a->ids[i] > b->ids[j+3]){ j+=4; continue; }
        }
        uint64_t x=a->ids[i], y=b->ids[j];
        if(x==y){ a->ids[w++]=x; i++; j++; }
        else if(x<y) i++;
        else j++;
    }
    a->n=w;
#else
    traditional_intersect(a,b);
#endif
}
#else
static int cpu_has_avx2(void){ return 0; }
static void intersect_avx2(IdVec* a, const IdVec* b){ traditional_intersect(a,b); }
#endif

#if defined(__aarch64__) || defined(__ARM_NEON)
#include <arm_neon.h>
#if defined(__linux__)
#include <sys/auxv.h>
#include <asm/hwcap.h>
#endif
static int cpu_has_neon(void){
#if defined(__aarch64__)
    return 1;
#elif defined(__linux__)
    return (getauxval(AT_HWCAP) & HWCAP_NEON) != 0;
#else
    return 0;
#endif
}
static void intersect_neon(IdVec* a, const IdVec* b){
#if defined(__ARM_NEON) || defined(__aarch64__)
    size_t i=0,j=0,w=0;
    while(i<a->n && j<b->n){
        if(j+2<=b->n){
            uint64x2_t bv=vld1q_u64(b->ids+j);
            uint64x2_t xv=vdupq_n_u64(a->ids[i]);
            uint64x2_t cmp=vceqq_u64(xv,bv);
            uint64_t m0=vgetq_lane_u64(cmp,0);
            uint64_t m1=vgetq_lane_u64(cmp,1);
            if(m0){
                a->ids[w++]=a->ids[i++];
                j+=1;
                continue;
            }
            if(m1){
                a->ids[w++]=a->ids[i++];
                j+=2;
                continue;
            }
            if(a->ids[i] > b->ids[j+1]){ j+=2; continue; }
        }
        uint64_t x=a->ids[i], y=b->ids[j];
        if(x==y){ a->ids[w++]=x; i++; j++; }
        else if(x<y) i++;
        else j++;
    }
    a->n=w;
#else
    traditional_intersect(a,b);
#endif
}
#else
static int cpu_has_neon(void){ return 0; }
static void intersect_neon(IdVec* a, const IdVec* b){ traditional_intersect(a,b); }
#endif

static void simd_intersect(IdVec* a, const IdVec* b){
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386) || defined(_M_IX86)
    static int use_avx2=-1;
    if(use_avx2==-1) use_avx2=cpu_has_avx2();
    if(use_avx2){ intersect_avx2(a,b); return; }
#elif defined(__aarch64__) || defined(__ARM_NEON)
    static int use_neon=-1;
    if(use_neon==-1) use_neon=cpu_has_neon();
    if(use_neon){ intersect_neon(a,b); return; }
#endif
    traditional_intersect(a,b);
}

static void galloping_intersect(IdVec* a, const IdVec* b){
    if(a->n==0 || b->n==0){ a->n=0; return; }
    if(a->n * 8 < b->n){
        size_t w=0, j=0;
        for(size_t i=0;i<a->n;i++){
            j += binary_search_ge(b->ids + j, b->n - j, a->ids[i]);
            if(j<b->n && b->ids[j]==a->ids[i]){ a->ids[w++]=a->ids[i]; j++; }
        }
        a->n=w;
    } else {
        simd_intersect(a, b);
    }
}

static void union_inplace(IdVec* a, const IdVec* b){
    if(b->n==0) return;
    size_t need = a->n + b->n;
    if(a->cap < need){
        a->cap = need;
        a->ids = (uint64_t*)realloc(a->ids, a->cap*sizeof(uint64_t));
    }
    // merge from the back to avoid overwriting data in a
    size_t i=a->n, j=b->n, k=need;
    while(i>0 && j>0){
        uint64_t x=a->ids[i-1], y=b->ids[j-1];
        uint64_t v;
        if(x>y){ v=x; i--; }
        else if(y>x){ v=y; j--; }
        else { v=x; i--; j--; }
        a->ids[--k]=v;
        while(i>0 && a->ids[i-1]==v) i--; // skip dups in a
        while(j>0 && b->ids[j-1]==v) j--; // skip dups in b
    }
    while(i>0){ uint64_t v=a->ids[i-1]; a->ids[--k]=v; while(i>0 && a->ids[i-1]==v) i--; }
    while(j>0){ uint64_t v=b->ids[j-1]; a->ids[--k]=v; while(j>0 && b->ids[j-1]==v) j--; }
    size_t outn = need - k;
    memmove(a->ids, a->ids+k, outn*sizeof(uint64_t));
    a->n = outn;
}

static void difference_inplace(IdVec* a, const IdVec* b){
    size_t i=0,j=0,w=0;
    while(i<a->n && j<b->n){
        uint64_t x=a->ids[i], y=b->ids[j];
        if(x==y){ i++; j++; }
        else if(x<y){ a->ids[w++]=x; i++; }
        else { j++; }
    }
    while(i<a->n){ a->ids[w++]=a->ids[i++]; }
    a->n=w;
}

// Collect candidate name string_ids via trigram intersection
static void collect_trigram_candidates(MDB_txn* txn, MDB_dbi dbi_trigram, const char* term, IdVec* out){
    // Restrict the length we process to avoid reading beyond the end of a
    // malformed input string.
    size_t len = strnlen(term, DB_BLOOM_MAX_BYTES);
    if(len < 3 || len == DB_BLOOM_MAX_BYTES){
        // fallback: too short or invalid; skip trigram and let filename_index iterate
        out->n=0; return;
    }
    char* tmp=(char*)_malloca(len+1);
    memcpy(tmp, term, len);
    tmp[len]='\0';
    lowercase_ascii(tmp, len);
    IdVec candidates; idvec_init(&candidates);
    for(size_t i=0;i+3<=len;i++){
        uint32_t key = ((uint8_t)tmp[i]<<16)|((uint8_t)tmp[i+1]<<8)|((uint8_t)tmp[i+2]);
        MDB_cursor* c=NULL; mdb_cursor_open(txn, dbi_trigram, &c);
        MDB_val k={.mv_data=&key,.mv_size=3}, v;
        IdVec gram; idvec_init(&gram);
        if(mdb_cursor_get(c, &k, &v, MDB_SET_KEY)==0){
            do{
                idvec_push(&gram, *(uint64_t*)v.mv_data);
            }while(mdb_cursor_get(c, &k, &v, MDB_NEXT_DUP)==0);
        }
        mdb_cursor_close(c);
        sort_unique(&gram);
        if(i==0){ candidates = gram; } else {
            sort_unique(&candidates);
            galloping_intersect(&candidates, &gram);
            idvec_free(&gram);
        }
        if(candidates.n==0) break;
    }
    *out = candidates;
    _freea(tmp);
}

static int precedence(Token t){
    if(t.type==TOK_NOT) return 3;
    if(t.type==TOK_AND) return 2;
    if(t.type==TOK_OR)  return 1;
    return 0;
}

static Node* make_leaf(Token t){
    Node* n=(Node*)calloc(1,sizeof(Node));
    n->type=TOK_TERM;
    n->ttype=t.ttype;
    n->text=t.text?_strdup(t.text):NULL;
    n->left=n->right=NULL;
    return n;
}

static void apply_op(Token op, Node** stack, int* sp){
    Node* n=(Node*)calloc(1,sizeof(Node));
    if(op.type==TOK_NOT){
        if(*sp<1){ free(n); return; }
        n->type=TOK_NOT;
        n->left=stack[--(*sp)];
        n->right=NULL;
    } else {
        if(*sp<2){ free(n); return; }
        Node* r=stack[--(*sp)];
        Node* l=stack[--(*sp)];
        n->type=op.type;
        n->left=l;
        n->right=r;
    }
    stack[(*sp)++]=n;
}

static Node* parse_tokens(const TokenList* toks){
    Token opstack[256]; int os=0;
    Node* nodestack[256]; int ns=0;
    for(int i=0;i<toks->n;i++){
        Token tk=toks->items[i];
        if(tk.type==TOK_TERM){
            nodestack[ns++]=make_leaf(tk);
        } else if(tk.type==TOK_AND || tk.type==TOK_OR || tk.type==TOK_NOT){
            while(os>0 && opstack[os-1].type!=TOK_LPAREN &&
                  precedence(opstack[os-1])>=precedence(tk)){
                apply_op(opstack[--os], nodestack, &ns);
            }
            opstack[os++]=tk;
        } else if(tk.type==TOK_LPAREN){
            opstack[os++]=tk;
        } else if(tk.type==TOK_RPAREN){
            while(os>0 && opstack[os-1].type!=TOK_LPAREN){
                apply_op(opstack[--os], nodestack, &ns);
            }
            if(os>0 && opstack[os-1].type==TOK_LPAREN) os--;
        }
    }
    while(os>0){ apply_op(opstack[--os], nodestack, &ns); }
    if(ns!=1) return NULL;
    return nodestack[0];
}

typedef void (*RecordCallback)(uint64_t id, void* ctx);

// Iterate all record IDs and invoke the callback for each.  This avoids
// materializing the full list of IDs in memory which can be several
// gigabytes for large databases.
static void stream_all_records(MDB_txn* txn, MDB_dbi dbi_date,
                               RecordCallback cb, void* ctx){
    MDB_cursor* cd=NULL;
    if(mdb_cursor_open(txn, dbi_date, &cd)!=0) return;
    MDB_val k,v; int rc=mdb_cursor_get(cd,&k,&v,MDB_FIRST);
    while(rc==0){
        cb(*(uint64_t*)v.mv_data, ctx);
        rc=mdb_cursor_get(cd,&k,&v,MDB_NEXT);
    }
    mdb_cursor_close(cd);
}

typedef struct { const IdVec* excl; IdVec* out; } DiffCtx;
static void diff_collect(uint64_t id, void* p){
    DiffCtx* c=(DiffCtx*)p;
    if(!bsearch(&id, c->excl->ids, c->excl->n, sizeof(uint64_t), cmp_u64))
        idvec_push(c->out, id);
}

static void collect_record(uint64_t id, void* ctx){
    idvec_push((IdVec*)ctx, id);
}

static void records_for_range(MDB_txn* txn, MDB_dbi dbi, uint64_t minv, uint64_t maxv, IdVec* out){
    MDB_cursor* c=NULL; mdb_cursor_open(txn, dbi, &c);
    MDB_val k={.mv_data=&minv,.mv_size=sizeof(minv)}, v;
    int rc = mdb_cursor_get(c,&k,&v,MDB_SET_RANGE);
    MDB_val maxk={.mv_data=&maxv,.mv_size=sizeof(maxv)};
    while(rc==0 && mdb_cmp(txn, dbi, &k, &maxk) <= 0){
        idvec_push(out, *(uint64_t*)v.mv_data);
        rc = mdb_cursor_get(c,&k,&v,MDB_NEXT);
    }
    mdb_cursor_close(c);
}

static void records_for_meta(MDB_txn* txn, MDB_dbi dbi_index, MDB_dbi dbi_strrev, const char* val, IdVec* out){
    MDB_val k={.mv_data=(void*)val,.mv_size=strlen(val)}, v;
    if(mdb_get(txn, dbi_strrev,&k,&v)==0){
        uint64_t sid=*(uint64_t*)v.mv_data;
        MDB_cursor* c=NULL; mdb_cursor_open(txn, dbi_index,&c);
        MDB_val ak={.mv_data=&sid,.mv_size=sizeof(sid)}, av;
        if(mdb_cursor_get(c,&ak,&av,MDB_SET_KEY)==0){
            do{ idvec_push(out, *(uint64_t*)av.mv_data); }
            while(mdb_cursor_get(c,&ak,&av,MDB_NEXT_DUP)==0);
        }
        mdb_cursor_close(c);
    }
}
static void records_for_author(MDB_txn* txn, MDB_dbi dbi_author, MDB_dbi dbi_strrev, const char* author, IdVec* out){
    records_for_meta(txn, dbi_author, dbi_strrev, author, out);
}
static void records_for_camera(MDB_txn* txn, MDB_dbi dbi_camera, MDB_dbi dbi_strrev, const char* camera, IdVec* out){
    records_for_meta(txn, dbi_camera, dbi_strrev, camera, out);
}
static void records_for_lens(MDB_txn* txn, MDB_dbi dbi_lens, MDB_dbi dbi_strrev, const char* lens, IdVec* out){
    records_for_meta(txn, dbi_lens, dbi_strrev, lens, out);
}
static void records_for_artist(MDB_txn* txn, MDB_dbi dbi_artist, MDB_dbi dbi_strrev, const char* artist, IdVec* out){
    records_for_meta(txn, dbi_artist, dbi_strrev, artist, out);
}
static void records_for_album(MDB_txn* txn, MDB_dbi dbi_album, MDB_dbi dbi_strrev, const char* album, IdVec* out){
    records_for_meta(txn, dbi_album, dbi_strrev, album, out);
}
static void records_for_title(MDB_txn* txn, MDB_dbi dbi_title, MDB_dbi dbi_strrev, const char* title, IdVec* out){
    records_for_meta(txn, dbi_title, dbi_strrev, title, out);
}

static void records_for_ext(MDB_txn* txn, MDB_dbi dbi_ext, const char* ext, IdVec* out){
    char buf[32];
    strncpy(buf, ext, 31);
    buf[31]=0;
    lowercase_ascii(buf, strlen(buf));
    MDB_cursor* ce=NULL; mdb_cursor_open(txn, dbi_ext, &ce);
    MDB_val k={.mv_data=buf,.mv_size=strlen(buf)}, v;
    if(mdb_cursor_get(ce,&k,&v,MDB_SET_KEY)==0){
        do{ idvec_push(out, *(uint64_t*)v.mv_data); }
        while(mdb_cursor_get(ce,&k,&v,MDB_NEXT_DUP)==0);
    }
    mdb_cursor_close(ce);
}

static void records_for_content(MDB_txn* txn, MDB_dbi dbi_trigram, MDB_dbi dbi_content, const char* term, IdVec* out){
    IdVec ids; idvec_init(&ids);
    collect_trigram_candidates(txn, dbi_trigram, term, &ids);
    sort_unique(&ids);
    MDB_cursor* cc=NULL; mdb_cursor_open(txn, dbi_content, &cc);
    for(size_t i=0;i<ids.n;i++){
        MDB_val k={.mv_data=&ids.ids[i],.mv_size=sizeof(uint64_t)}, v;
        if(mdb_cursor_get(cc,&k,&v,MDB_SET_KEY)==0){
            do{ idvec_push(out, *(uint64_t*)v.mv_data); }
            while(mdb_cursor_get(cc,&k,&v,MDB_NEXT_DUP)==0);
        }
    }
    mdb_cursor_close(cc);
    idvec_free(&ids);
}

static const size_t MAX_NAME_RESULTS = 100000;


static void records_for_name(MDB_txn* txn, MDB_dbi dbi_trigram, MDB_dbi dbi_fname, MDB_dbi dbi_strings, MDB_dbi dbi_smeta, const char* term, IdVec* out){
    IdVec name_ids; idvec_init(&name_ids);
    size_t raw_len = strnlen(term, DB_BLOOM_MAX_BYTES);
    if(raw_len == DB_BLOOM_MAX_BYTES){ idvec_free(&name_ids); return; }
    char* normalized = (char*)malloc(raw_len + 1);
    if(!normalized){ idvec_free(&name_ids); return; }
    normalize_filename_utf8(term, normalized, raw_len + 1);
    size_t normalized_len = strlen(normalized);
    collect_trigram_candidates(txn, dbi_trigram, normalized, &name_ids);
    sort_unique(&name_ids);
    if(name_ids.n>0){
        size_t keep=0;
        size_t tlen = normalized_len;
        const char* tl = normalized;
        uint32_t hbuf[4096]; size_t hn=0;
        size_t limit = tlen > DB_BLOOM_MAX_BYTES ? DB_BLOOM_MAX_BYTES : tlen;
        size_t full = limit < DB_BLOOM_STRIDE_AFTER ? limit : DB_BLOOM_STRIDE_AFTER;
        for(size_t i=0;i+3<=full && hn<4096;i++){
            uint32_t h=2166136261u ^ 0xA5A5A5A5u; for(int k=0;k<3;k++){ h^=(uint8_t)tl[i+k]; h*=16777619u; } hbuf[hn++]=h;
            h=2166136261u ^ 0x3C3C3C3Cu; for(int k=0;k<3;k++){ h^=(uint8_t)tl[i+k]; h*=16777619u; } hbuf[hn++]=h;
            h=2166136261u ^ 0x5A5A5A5Au; for(int k=0;k<3;k++){ h^=(uint8_t)tl[i+k]; h*=16777619u; } hbuf[hn++]=h;
            h=2166136261u ^ 0x1F1F1F1Fu; for(int k=0;k<3;k++){ h^=(uint8_t)tl[i+k]; h*=16777619u; } hbuf[hn++]=h;
        }
        for(size_t i=full;i+3<=limit && hn<4096;i+=DB_BLOOM_STRIDE){
            uint32_t h=2166136261u ^ 0xA5A5A5A5u; for(int k=0;k<3;k++){ h^=(uint8_t)tl[i+k]; h*=16777619u; } hbuf[hn++]=h;
            h=2166136261u ^ 0x3C3C3C3Cu; for(int k=0;k<3;k++){ h^=(uint8_t)tl[i+k]; h*=16777619u; } hbuf[hn++]=h;
            h=2166136261u ^ 0x5A5A5A5Au; for(int k=0;k<3;k++){ h^=(uint8_t)tl[i+k]; h*=16777619u; } hbuf[hn++]=h;
            h=2166136261u ^ 0x1F1F1F1Fu; for(int k=0;k<3;k++){ h^=(uint8_t)tl[i+k]; h*=16777619u; } hbuf[hn++]=h;
        }
        for(size_t i=0;i<name_ids.n;i++){
            MDB_val key={.mv_data=&name_ids.ids[i],.mv_size=sizeof(uint64_t)}, val;
            if(mdb_get(txn, dbi_strings,&key,&val)!=0) continue;
            MDB_val text_val; StringMeta inline_meta;
            BOOL has_inline = string_value_parse(&val, &text_val, &inline_meta);
            const StringMeta* sm = has_inline ? &inline_meta : NULL;
            StringMeta legacy_meta;
            if(!sm && dbi_smeta != MDB_DBI_INVALID){
                MDB_val mv;
                if(mdb_get(txn, dbi_smeta, &key, &mv)==0 && mv.mv_size>=sizeof(StringMeta)){
                    memcpy(&legacy_meta, mv.mv_data, sizeof(StringMeta));
                    sm = &legacy_meta;
                }
            }
            BOOL ok = FALSE;
            if(sm && sm->bloom_pending && text_val.mv_size >= 5){
                bloom_generator_request(name_ids.ids[i]);
                ok = string_contains_lower_term(&text_val, tl);
            }
            else if(sm && !sm->bloom_pending && sm->bloom_offset < g_bloom_size && g_bloom_size - sm->bloom_offset >= sm->bloom_length){
                size_t bloom_bytes = 0;
                uint8_t* bloom = bloom_cache_get(name_ids.ids[i], sm, &bloom_bytes);
                if(bloom){
                    uint32_t mask = string_meta_bloom_mask(sm);
                    if(mask == 0 && bloom_bytes){ mask = (uint32_t)(bloom_bytes * 8 - 1); }
                    uint32_t hash_to_check = sm->hash_count;
                    if(hash_to_check == 0 || hash_to_check > 4) hash_to_check = 4;
                    if(mask){
                        ok = TRUE;
                        for(size_t base=0;base<hn && ok;base+=4){
                            for(uint32_t h=0; h<hash_to_check && base + h < hn; ++h){
                                uint32_t bit = hbuf[base + h] & mask;
                                if((bloom[bit>>3] & (uint8_t)(1u << (bit & 7))) == 0){ ok = FALSE; break; }
                            }
                        }
                    }
                }
            }
            if(!ok){
                ok = string_contains_lower_term(&text_val, tl);
            }
            if(ok){ name_ids.ids[keep++]=name_ids.ids[i]; }
        }
        name_ids.n=keep;
    }
    MDB_cursor* cix=NULL; mdb_cursor_open(txn, dbi_fname, &cix);
    BOOL limit_reached = FALSE;
    if(name_ids.n>0){
        for(size_t i=0;i<name_ids.n && !limit_reached;i++){
            MDB_val k={.mv_data=&name_ids.ids[i],.mv_size=sizeof(uint64_t)}, v;
            if(mdb_cursor_get(cix,&k,&v,MDB_SET_KEY)==0){
                do{
                    if(out->n >= MAX_NAME_RESULTS){ limit_reached = TRUE; break; }
                    idvec_push(out, *(uint64_t*)v.mv_data);
                }
                while(!limit_reached && mdb_cursor_get(cix,&k,&v,MDB_NEXT_DUP)==0);
            }
        }
    } else {
        int maxd=(int)((normalized_len+4)/5);
        MDB_val key,val;
        int rc=mdb_cursor_get(cix,&key,&val,MDB_FIRST);
        while(rc==0 && !limit_reached){
            uint64_t sid=*(uint64_t*)key.mv_data;
            MDB_val sk={.mv_data=&sid,.mv_size=sizeof(sid)}, sv;
            if(mdb_get(txn, dbi_strings, &sk, &sv)!=0){
                rc = mdb_cursor_get(cix,&key,&val,MDB_NEXT_NODUP);
                continue;
            }
            MDB_val text_val; StringMeta meta_tmp;
            string_value_parse(&sv, &text_val, &meta_tmp);
            char* tmp=(char*)_malloca(text_val.mv_size + 1);
            if(!tmp){ rc = mdb_cursor_get(cix,&key,&val,MDB_NEXT_NODUP); continue; }
            memcpy(tmp,text_val.mv_data,text_val.mv_size); tmp[text_val.mv_size]=0;
            BOOL matched = fuzzy_match(tmp, normalized, maxd);
            _freea(tmp);
            if(matched){
                do{
                    if(out->n >= MAX_NAME_RESULTS){ limit_reached = TRUE; break; }
                    idvec_push(out, *(uint64_t*)val.mv_data);
                }
                while(!limit_reached && mdb_cursor_get(cix,&key,&val,MDB_NEXT_DUP)==0);
                if(!limit_reached){
                    rc = mdb_cursor_get(cix,&key,&val,MDB_NEXT_NODUP);
                }
            } else {
                rc = mdb_cursor_get(cix,&key,&val,MDB_NEXT_NODUP);
            }
        }
    }
    mdb_cursor_close(cix);
    idvec_free(&name_ids);
    free(normalized);
}

static void eval_node(Node* n, MDB_txn* txn, MDB_dbi dbi_strings, MDB_dbi dbi_fname, MDB_dbi dbi_trigram, MDB_dbi dbi_smeta, MDB_dbi dbi_content, MDB_dbi dbi_author, MDB_dbi dbi_camera, MDB_dbi dbi_lens, MDB_dbi dbi_artist, MDB_dbi dbi_album, MDB_dbi dbi_title, MDB_dbi dbi_ext, MDB_dbi dbi_strrev, MDB_dbi dbi_date, IdVec* out){
    if(!n) return;
    if(n->type==TOK_TERM){
        if(!try_load_term_cache(n->ttype, n->text, out)){
            switch(n->ttype){
                case TERM_NAME: records_for_name(txn, dbi_trigram, dbi_fname, dbi_strings, dbi_smeta, n->text, out); break;
                case TERM_CONTENT: records_for_content(txn, dbi_trigram, dbi_content, n->text, out); break;
                case TERM_AUTHOR: records_for_author(txn, dbi_author, dbi_strrev, n->text, out); break;
                case TERM_CAMERA: records_for_camera(txn, dbi_camera, dbi_strrev, n->text, out); break;
                case TERM_LENS: records_for_lens(txn, dbi_lens, dbi_strrev, n->text, out); break;
                case TERM_ARTIST: records_for_artist(txn, dbi_artist, dbi_strrev, n->text, out); break;
                case TERM_ALBUM: records_for_album(txn, dbi_album, dbi_strrev, n->text, out); break;
                case TERM_TITLE: records_for_title(txn, dbi_title, dbi_strrev, n->text, out); break;
                case TERM_EXT: records_for_ext(txn, dbi_ext, n->text, out); break;
            }
            sort_unique(out);
            save_term_cache(n->ttype, n->text, out);
        } else {
            sort_unique(out);
        }
        return;
    }
    if(n->type==TOK_AND || n->type==TOK_OR){
        IdVec L; idvec_init(&L); IdVec R; idvec_init(&R);
        eval_node(n->left, txn, dbi_strings, dbi_fname, dbi_trigram, dbi_smeta, dbi_content, dbi_author, dbi_camera, dbi_lens, dbi_artist, dbi_album, dbi_title, dbi_ext, dbi_strrev, dbi_date, &L);
        eval_node(n->right, txn, dbi_strings, dbi_fname, dbi_trigram, dbi_smeta, dbi_content, dbi_author, dbi_camera, dbi_lens, dbi_artist, dbi_album, dbi_title, dbi_ext, dbi_strrev, dbi_date, &R);
        sort_unique(&L); sort_unique(&R);
        if(n->type==TOK_AND){ galloping_intersect(&L,&R); } else { union_inplace(&L,&R); }
        idvec_free(&R);
        *out=L; return;
    }
    if(n->type==TOK_NOT){
        IdVec B; idvec_init(&B);
        eval_node(n->left, txn, dbi_strings, dbi_fname, dbi_trigram, dbi_smeta,
                  dbi_content, dbi_author, dbi_camera, dbi_lens, dbi_artist,
                  dbi_album, dbi_title, dbi_ext, dbi_strrev, dbi_date, &B);
        sort_unique(&B);
        IdVec All; idvec_init(&All);
        DiffCtx ctx = { .excl=&B, .out=&All };
        stream_all_records(txn, dbi_date, diff_collect, &ctx);
        idvec_free(&B);
        *out=All; return;
    }
}

// Main search
int wmain(int argc, wchar_t** argv){
    config_init_default();
    config_load_file(L"anything.conf");
    // Build canonical query string (all args except --db <path>)
    char qcanon[4096]={0}; size_t qpos=0;
    char qcanon_base[4096]={0}; size_t qbase=0;
    for(int i=1;i<argc;i++){
        if(wcscmp(argv[i], L"--db")==0){ i++; continue; }
        if(wcscmp(argv[i], L"--workers")==0){ i++; continue; }
        if(wcscmp(argv[i], L"--json")==0){ continue; }
        if(wcscmp(argv[i], L"--start-indexer")==0 || wcscmp(argv[i], L"--pause-indexer")==0){ continue; }
        char u8[512]; to_utf8(argv[i], u8, sizeof(u8));
        size_t ulen = strlen(u8);
        if(qpos + ulen + 2 < sizeof(qcanon)){ memcpy(qcanon+qpos,u8,ulen); qpos+=ulen; qcanon[qpos++]=' '; qcanon[qpos]=0; }
        BOOL is_filter = (_strnicmp(u8,"size:",5)==0)||(_strnicmp(u8,"dm:",3)==0)||(_strnicmp(u8,"path:",5)==0);
        if(!is_filter){
            if(qbase + ulen + 2 < sizeof(qcanon_base)){
                memcpy(qcanon_base+qbase,u8,ulen);
                qbase+=ulen; qcanon_base[qbase++]=' '; qcanon_base[qbase]=0;
            }
        }
    }
    enterprise_audit_log("user", qcanon);
    enterprise_ad_authenticate("user", "");
    wchar_t dbPath[MAX_LONG_PATH];
    SearchQuery q; TokenList tokens;
    int workers = g_config.default_search_workers; bool json_output=false;
    bool admin_start=false, admin_pause=false;
    for(int ai=1; ai<argc; ++ai){
        if(wcscmp(argv[ai], L"--workers")==0 && ai+1<argc){
            workers = _wtoi(argv[++ai]);
        } else if(wcscmp(argv[ai], L"--json")==0){
            json_output=true;
        } else if(wcscmp(argv[ai], L"--start-indexer")==0){
            admin_start=true;
        } else if(wcscmp(argv[ai], L"--pause-indexer")==0){
            admin_pause=true;
        }
    }
    if(workers<1) workers=1;
    if(workers>g_config.max_search_workers) workers=g_config.max_search_workers;
    parse_query(argc, argv, dbPath, &q, &tokens);
    if(!dbPath[0]){ if(json_output) output_error(json_output, "missing --db"); else usage(); free_search_query(&q); tokenlist_free(&tokens); return 1; }
    if(!enterprise_check_permission("user", "db")){
        output_error(json_output, "Permission denied");
        free_search_query(&q); tokenlist_free(&tokens); return 1;
    }
    if(admin_start || admin_pause){
        int rc = set_indexer_state(dbPath, admin_start, json_output);
        free_search_query(&q); tokenlist_free(&tokens); return rc;
    }
    wcscpy_s(g_db_path, MAX_LONG_PATH, dbPath);
    Db* hdr_db = NULL;
    const DbHeader* hdr = db_open_readonly(dbPath, &hdr_db);
    if(!hdr){ output_error(json_output, "db open failed"); free_search_query(&q); tokenlist_free(&tokens); return 1; }
    g_db_generation = hdr->generation;
    db_close(hdr_db);
    if(!open_bloom(dbPath)){ output_error(json_output, "bloom open failed"); free_search_query(&q); tokenlist_free(&tokens); return 1; }

    // Try cache based on query without size/path/date filters
    IdVec rec_ids; idvec_init(&rec_ids);
    BOOL cache_hit = try_load_cache(dbPath, qcanon_base, &rec_ids);

    // Open env and dbis
    MDB_env* env=NULL; MDB_txn* txn=NULL;
    if(mdb_env_create(&env)!=0){ output_error(json_output,"env_create failed"); free_search_query(&q); tokenlist_free(&tokens); close_bloom(); return 1; }
    mdb_env_set_maxdbs(env, 64);
    char u8db[MAX_LONG_PATH*3]; to_utf8(dbPath,u8db,sizeof(u8db));
    if(mdb_env_open(env, u8db, MDB_RDONLY, 0664)!=0){ output_error(json_output,"env_open failed"); mdb_env_close(env); free_search_query(&q); tokenlist_free(&tokens); close_bloom(); return 1; }
    if(mdb_txn_begin(env,NULL,MDB_RDONLY,&txn)!=0){ output_error(json_output,"txn_begin failed"); mdb_env_close(env); close_bloom(); free_search_query(&q); tokenlist_free(&tokens); return 1; }
    MDB_dbi dbi_strings, dbi_records, dbi_fname_index, dbi_trigram, dbi_size, dbi_mtime, dbi_date, dbi_ext, dbi_smeta, dbi_content, dbi_author, dbi_camera, dbi_lens, dbi_artist, dbi_album, dbi_title, dbi_strrev, dbi_attr;
    if(mdb_dbi_open(txn,"strings",0,&dbi_strings)!=0 ||
       mdb_dbi_open(txn,"records",0,&dbi_records)!=0 ||
       mdb_dbi_open(txn,"filename_index",0,&dbi_fname_index)!=0 ||
       mdb_dbi_open(txn,"trigram_index",0,&dbi_trigram)!=0 ||
       mdb_dbi_open(txn,"size_index",0,&dbi_size)!=0 ||
       mdb_dbi_open(txn,"mtime_index",0,&dbi_mtime)!=0 ||
       mdb_dbi_open(txn,"date_index",0,&dbi_date)!=0 ||
       mdb_dbi_open(txn,"extension_index",0,&dbi_ext)!=0){
       output_error(json_output,"dbi_open failed"); mdb_txn_abort(txn); mdb_env_close(env); close_bloom(); free_search_query(&q); tokenlist_free(&tokens); return 1;
    }
    dbi_smeta = MDB_DBI_INVALID;
    int rc_smeta = mdb_dbi_open(txn,"string_meta",0,&dbi_smeta);
    if(rc_smeta == MDB_NOTFOUND){
        dbi_smeta = MDB_DBI_INVALID;
    } else if(rc_smeta != 0){
       output_error(json_output,"dbi_open failed"); mdb_txn_abort(txn); mdb_env_close(env); close_bloom(); free_search_query(&q); tokenlist_free(&tokens); return 1;
    }
    if(mdb_dbi_open(txn,"content_index",0,&dbi_content)!=0 ||
       mdb_dbi_open(txn,"author_index",0,&dbi_author)!=0 ||
       mdb_dbi_open(txn,"camera_index",0,&dbi_camera)!=0 ||
       mdb_dbi_open(txn,"lens_index",0,&dbi_lens)!=0 ||
       mdb_dbi_open(txn,"artist_index",0,&dbi_artist)!=0 ||
       mdb_dbi_open(txn,"album_index",0,&dbi_album)!=0 ||
       mdb_dbi_open(txn,"title_index",0,&dbi_title)!=0 ||
       mdb_dbi_open(txn,"strrev",0,&dbi_strrev)!=0 ||
       mdb_dbi_open(txn,"attr_index",0,&dbi_attr)!=0){
       output_error(json_output,"dbi_open failed"); mdb_txn_abort(txn); mdb_env_close(env); close_bloom(); free_search_query(&q); tokenlist_free(&tokens); return 1;
    }


    IdVec cache_copy; idvec_init(&cache_copy);
    if(!cache_hit){
        Node* root = parse_tokens(&tokens);
        if(root){
            eval_node(root, txn, dbi_strings, dbi_fname_index, dbi_trigram, dbi_smeta, dbi_content, dbi_author, dbi_camera, dbi_lens, dbi_artist, dbi_album, dbi_title, dbi_ext, dbi_strrev, dbi_date, &rec_ids);
            free_node(root);
        } else {
            stream_all_records(txn, dbi_date, collect_record, &rec_ids);
        }
        sort_unique(&rec_ids);
        cache_copy.ids = (uint64_t*)malloc(rec_ids.n*sizeof(uint64_t));
        memcpy(cache_copy.ids, rec_ids.ids, rec_ids.n*sizeof(uint64_t));
        cache_copy.n = cache_copy.cap = rec_ids.n;
    } else {
        sort_unique(&rec_ids);
    }

    // Use secondary indexes to restrict candidate IDs
    if(q.size_min>0 || q.size_max<~0ULL){
        IdVec sz; idvec_init(&sz);
        records_for_range(txn, dbi_size, q.size_min, q.size_max, &sz);
        if(rec_ids.n>0){ galloping_intersect(&rec_ids,&sz); idvec_free(&sz); }
        else { rec_ids = sz; }
    }
    if(q.date_min_day>0 || q.date_max_day<~0ULL){
        uint64_t minft = day_to_filetime(q.date_min_day);
        uint64_t maxft = day_to_filetime(q.date_max_day+1) - 1;
        IdVec dt; idvec_init(&dt);
        records_for_range(txn, dbi_mtime, minft, maxft, &dt);
        if(rec_ids.n>0){ galloping_intersect(&rec_ids,&dt); idvec_free(&dt); }
        else { rec_ids = dt; }
    }

    MDB_stat st; mdb_stat(txn, dbi_records, &st);
    size_t total_docs = st.ms_entries;
    size_t docs_with_term = rec_ids.n;

    // Step 3: Apply filters, rank, and print (parallel)
    int tcount = workers;
    HANDLE th[4]; FilterArgs fa[4];
    for(int ti=0; ti<tcount; ++ti){
        size_t s = (rec_ids.n*ti)/tcount;
        size_t e = (rec_ids.n*(ti+1))/tcount;
        ZeroMemory(&fa[ti], sizeof(FilterArgs));
        fa[ti].ids = rec_ids.ids;
        fa[ti].start = s;
        fa[ti].end = e;
        fa[ti].q = &q;
        fa[ti].outcap = e - s;
        fa[ti].out = (RankedResult*)malloc(fa[ti].outcap * sizeof(RankedResult));
        fa[ti].outn = 0;
        fa[ti].total_docs = total_docs;
        fa[ti].docs_with_term = docs_with_term;
        fa[ti].env = env;
        fa[ti].dbi_strings = dbi_strings;
        fa[ti].dbi_records = dbi_records;
        uintptr_t h = _beginthreadex(NULL,0,(unsigned (__stdcall *)(void*))filter_worker_thread,&fa[ti],0,NULL);
        th[ti] = (HANDLE)h;
    }
    WaitForMultipleObjects(tcount, th, TRUE, INFINITE);
    for(int ti=0; ti<tcount; ++ti) CloseHandle(th[ti]);
    size_t alln = 0;
    for(int ti=0; ti<tcount; ++ti) alln += fa[ti].outn;
    RankedResult* all = (RankedResult*)malloc(alln * sizeof(RankedResult));
    size_t pos = 0;
    for(int ti=0; ti<tcount; ++ti){
        memcpy(all + pos, fa[ti].out, fa[ti].outn * sizeof(RankedResult));
        pos += fa[ti].outn;
        free(fa[ti].out);
    }
    // sort by score desc
    qsort(all, alln, sizeof(RankedResult), cmp_rank);
    size_t show = alln<200? alln:200;
    // Open txn once to print resolved strings
    MDB_txn* txnprint; mdb_txn_begin(env,NULL,MDB_RDONLY,&txnprint);
    MDB_dbi dbi_stringsP, dbi_recordsP; mdb_dbi_open(txnprint,"strings",0,&dbi_stringsP); mdb_dbi_open(txnprint,"records",0,&dbi_recordsP);
    if(json_output) printf("[\n");
    for(size_t i2=0;i2<show;i2++){
        uint64_t rid = all[i2].rec_id; MDB_val rk={.mv_data=&rid,.mv_size=sizeof(rid)}, rv;
        if(mdb_get(txnprint, dbi_recordsP, &rk, &rv)!=0 || rv.mv_size<sizeof(DbRecord)) continue;
        DbRecord* r = (DbRecord*)rv.mv_data;
        MDB_val pk={.mv_data=&r->parent_str_id,.mv_size=sizeof(r->parent_str_id)}, pv;
        MDB_val nk={.mv_data=&r->name_str_id,.mv_size=sizeof(r->name_str_id)}, nv;
        const char *pstr="?", *nstr="?";
        if(mdb_get(txnprint, dbi_stringsP, &pk, &pv)==0) pstr=(const char*)pv.mv_data;
        if(mdb_get(txnprint, dbi_stringsP, &nk, &nv)==0) nstr=(const char*)nv.mv_data;
        char full_path[MAX_LONG_PATH*3];
        snprintf(full_path, sizeof(full_path), "%s\\%s", pstr, nstr);
        if(!enterprise_check_permission("user", full_path)) continue;
        if(json_output){
            printf("  {\"path\":");
            print_json_path(pstr, nstr);
            printf(",\"size\":%llu,\"mtime\":%llu,\"score\":%.1f}%s\n",
                   (unsigned long long)r->file_size,
                   (unsigned long long)r->modified_time,
                   all[i2].score,
                   (i2+1<show)?",":"");
        } else {
            printf("%s\\%s  size=%llu  mtime=%llu  score=%.1f\n",
                   pstr, nstr,
                   (unsigned long long)r->file_size,
                   (unsigned long long)r->modified_time,
                   all[i2].score);
        }
    }
    if(json_output) printf("]\n");
    mdb_txn_abort(txnprint);
    free(all);

    // Save cache for next run using filterless query
    if(!cache_hit && cache_copy.n>0){
        save_cache(dbPath, qcanon_base, &cache_copy);
    }

    mdb_txn_abort(txn); mdb_env_close(env);

    // cleanup
    free_search_query(&q);
    idvec_free(&rec_ids);
    idvec_free(&cache_copy);
    tokenlist_free(&tokens);
    close_bloom();
    return 0;
}
