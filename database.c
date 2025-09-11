
// database.c — LMDB store with multiple indices and trigram index
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <wchar.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shlwapi.h>
#include <immintrin.h>
#pragma comment(lib, "shlwapi.lib")

#include "database.h"
#include "anything.h"
#include "util.h"
#include "lmdb.h"

#ifndef _strdup
#define _strdup strdup
#endif

typedef struct {
    uint32_t trigram_count;
    uint64_t bloom_offset; // offset in bloom file
} StringMeta;

typedef struct {
    uint64_t hash;
    uint64_t string_id;
    char* string;
} StringCache;

#define STRING_CACHE_SIZE 65536u
#define STRING_CACHE_MASK (STRING_CACHE_SIZE-1u)

static StringCache g_string_cache[STRING_CACHE_SIZE];

static uint64_t string_cache_lookup(const char* s, uint64_t h){
    for(uint32_t i=0;i<STRING_CACHE_SIZE;i++){
        StringCache* c = &g_string_cache[(h + i) & STRING_CACHE_MASK];
        if(!c->string) return 0;
        if(c->hash==h && strcmp(c->string, s)==0) return c->string_id;
    }
    return 0;
}

static void string_cache_insert(const char* s, uint64_t h, uint64_t id){
    for(uint32_t i=0;i<STRING_CACHE_SIZE;i++){
        StringCache* c = &g_string_cache[(h + i) & STRING_CACHE_MASK];
        if(!c->string){
            c->hash=h; c->string_id=id; c->string=_strdup(s); return;
        }
        if(c->hash==h && strcmp(c->string,s)==0){ c->string_id=id; return; }
    }
    StringCache* c = &g_string_cache[h & STRING_CACHE_MASK];
    free(c->string);
    c->hash=h; c->string_id=id; c->string=_strdup(s);
}

static inline void bloom_set(uint8_t* bloom, uint32_t h){
    uint32_t bit = h & 0xFFFFu; // 65536 bits
    bloom[bit>>3] |= (uint8_t)(1u << (bit & 7));
}
static inline BOOL bloom_has(const uint8_t* bloom, uint32_t h){
    uint32_t bit = h & 0xFFFFu;
    return (bloom[bit>>3] & (uint8_t)(1u << (bit & 7))) != 0;
}
static uint32_t hash32_seed(const void* data, size_t len, uint32_t seed){
    const uint8_t* p=(const uint8_t*)data;
    uint32_t h=2166136261u ^ seed;
    for(size_t i=0;i<len;i++){ h ^= p[i]; h *= 16777619u; }
    return h;
}

static uint32_t optimal_hash_count(size_t string_len, size_t bloom_bits){
    if(string_len == 0) return 1;
    uint32_t k = (uint32_t)(((double)bloom_bits / (double)string_len) * 0.693147);
    if(k == 0) k = 1;
    return k;
}
static void build_bloom_hashes_simd(const char* tri, uint32_t* out4){
    __m128i seeds = _mm_set_epi32(0x1F1F1F1F, 0x5A5A5A5A, 0x3C3C3C3C, 0xA5A5A5A5);
    uint32_t t= (uint8_t)tri[0] | ((uint32_t)(uint8_t)tri[1]<<8) | ((uint32_t)(uint8_t)tri[2]<<16);
    __m128i data = _mm_set1_epi32((int)t);
    __m128i x = _mm_xor_si128(data, seeds);
    __m128i mul = _mm_set1_epi32(16777619);
    __m128i res = _mm_mullo_epi32(x, mul);
    _mm_storeu_si128((__m128i*)out4, res);
}
static uint32_t build_bloom_for_name(const char* name_u8, uint8_t* bloom){
    // Bloom construction can be expensive on very large strings.  To keep the
    // cost bounded we only process the first DB_BLOOM_MAX_BYTES bytes and, after
    // DB_BLOOM_STRIDE_AFTER, we sample every DB_BLOOM_STRIDE-th trigram.
    if(!name_u8 || !bloom) return 0;
    ZeroMemory(bloom, 8192);
    uint32_t tcount = 0;
    size_t len = strlen(name_u8);
    if(len < 3) return 0;

    size_t limit = len > DB_BLOOM_MAX_BYTES ? DB_BLOOM_MAX_BYTES : len;
    char stack_tmp[512];
    BOOL heap = limit + 1 > sizeof(stack_tmp);
    char* tmp = heap ? (char*)malloc(limit + 1) : stack_tmp;
    if(!tmp) return 0;
    memcpy(tmp, name_u8, limit);
    tmp[limit] = '\0';
    lowercase_ascii(tmp, limit);

    size_t full = limit < DB_BLOOM_STRIDE_AFTER ? limit : DB_BLOOM_STRIDE_AFTER;

    size_t tri_full = full >= 3 ? (full - 2) : 0;
    size_t tri_sampled = 0;
    if(limit > full){
        size_t rem = limit - full;
        if(rem > 3) tri_sampled = ((rem - 3) / DB_BLOOM_STRIDE) + 1;
    }
    size_t tri_est = tri_full + tri_sampled;
    uint32_t hash_count = optimal_hash_count(tri_est ? tri_est : 1, 65536);
    if(hash_count > 4) hash_count = 4;

    for(size_t i=0; i + 3 <= full; ++i){
        uint32_t hs[4];
        build_bloom_hashes_simd(&tmp[i], hs);
        for(uint32_t j=0; j<hash_count; ++j){ bloom_set(bloom, hs[j]); }
        tcount++;
    }
    for(size_t i=full; i + 3 <= limit; i += DB_BLOOM_STRIDE){
        uint32_t hs[4];
        build_bloom_hashes_simd(&tmp[i], hs);
        for(uint32_t j=0; j<hash_count; ++j){ bloom_set(bloom, hs[j]); }
        tcount++;
    }
    if(heap) free(tmp);
    return tcount;
}


#define ALIGN_UP(x,a)   (((x)+(a)-1) & ~((a)-1))

typedef struct {
    MDB_env* env;
    MDB_dbi  dbi_meta;
    MDB_dbi  dbi_strings;
    MDB_dbi  dbi_strrev;
    MDB_dbi  dbi_records;
    MDB_dbi  dbi_fname_index;
    MDB_dbi  dbi_parent_index;
    MDB_dbi  dbi_size_index;      // key: uint64 size        → rec_id (dups)
    MDB_dbi  dbi_date_index;      // key: uint64 day         → rec_id (dups)
    MDB_dbi  dbi_mtime_index;     // key: uint64 modified    → rec_id (dups)
    MDB_dbi  dbi_attr_index;      // key: uint32 attributes  → rec_id (dups)
    MDB_dbi  dbi_extension_index; // key: utf8 ext   → rec_id (dups)
    MDB_dbi  dbi_path_hierarchy;  // key: parent_id  → rec_id (dups)
    MDB_dbi  dbi_trigram_index;   // key: 3 bytes    → string_id (dups)
    MDB_dbi  dbi_string_meta;    // key: string_id   → bloom+meta
    MDB_dbi  dbi_content_index;  // key: content_str_id → rec_id (dups)
    MDB_dbi  dbi_author_index;   // key: author_str_id  → rec_id (dups)
    MDB_dbi  dbi_camera_index;   // key: camera_str_id  → rec_id (dups)
    MDB_dbi  dbi_lens_index;     // key: lens_str_id    → rec_id (dups)
    MDB_dbi  dbi_artist_index;   // key: artist_str_id  → rec_id (dups)
    MDB_dbi  dbi_album_index;    // key: album_str_id   → rec_id (dups)
    MDB_dbi  dbi_title_index;    // key: title_str_id   → rec_id (dups)
    MDB_txn* wtxn;
    DbError  last_error;
    size_t   map_init;
    size_t   map_max;
    HANDLE   bloom_file;
    uint64_t bloom_offset;
    BOOL     dirty;
    DbHeader header_cache;
    IndexLoadState load_state;
} DbImpl;

static void set_error(DbImpl* d, DbErrorCode code, int detail, const char* msg){
    if(!d) return;
    d->last_error.code = code;
    d->last_error.detail = detail;
    if(msg){
        snprintf(d->last_error.message, sizeof(d->last_error.message), "%s", msg);
    } else {
        d->last_error.message[0] = '\0';
    }
}
static void set_mdb_error(DbImpl* d, int rc){
    set_error(d, DB_ERROR_LMDB, rc, mdb_strerror(rc));
}
static void set_sys_error(DbImpl* d, DWORD err){
    char buf[256];
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,NULL,err,0,buf,sizeof(buf),NULL);
    set_error(d, DB_ERROR_OS, (int)err, buf);
}
static uint64_t now_filetime(void) {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u; u.LowPart=ft.dwLowDateTime; u.HighPart=ft.dwHighDateTime;
    return u.QuadPart;
}
static void to_mdb_val(const void* p, size_t n, MDB_val* mv){ mv->mv_data=(void*)p; mv->mv_size=(size_t)n; }

static BOOL ensure_dir(const wchar_t* path){
    wchar_t tmp[MAX_PATH]; wcsncpy_s(tmp, MAX_PATH, path, _TRUNCATE);
    wchar_t* p = wcsrchr(tmp, L'\\'); if(p){ *p=0; CreateDirectoryW(tmp, NULL); }
    return TRUE;
}

static int open_core_dbs(MDB_txn* txn, DbImpl* d, BOOL create){
    unsigned int flags = create? MDB_CREATE:0;
    int rc = mdb_dbi_open(txn, "meta", flags, &d->dbi_meta);
    if(rc) return rc;
    if((rc = mdb_dbi_open(txn, "strings", flags, &d->dbi_strings))) return rc;
    if((rc = mdb_dbi_open(txn, "strrev", flags, &d->dbi_strrev))) return rc;
    if((rc = mdb_dbi_open(txn, "records", flags, &d->dbi_records))) return rc;
    if((rc = mdb_dbi_open(txn, "filename_index", flags|MDB_DUPSORT, &d->dbi_fname_index))) return rc;
    if((rc = mdb_dbi_open(txn, "parent_index", flags|MDB_DUPSORT, &d->dbi_parent_index))) return rc;
    return 0;
}

static int open_metadata_dbs(MDB_txn* txn, DbImpl* d, BOOL create){
    unsigned int flags = create? MDB_CREATE:0;
    int rc;
    if((rc = mdb_dbi_open(txn, "size_index", flags|MDB_DUPSORT|MDB_INTEGERKEY, &d->dbi_size_index))) return rc;
    if((rc = mdb_dbi_open(txn, "date_index", flags|MDB_DUPSORT|MDB_INTEGERKEY, &d->dbi_date_index))) return rc;
    if((rc = mdb_dbi_open(txn, "mtime_index", flags|MDB_DUPSORT|MDB_INTEGERKEY, &d->dbi_mtime_index))) return rc;
    if((rc = mdb_dbi_open(txn, "attr_index", flags|MDB_DUPSORT|MDB_INTEGERKEY, &d->dbi_attr_index))) return rc;
    if((rc = mdb_dbi_open(txn, "extension_index", flags|MDB_DUPSORT, &d->dbi_extension_index))) return rc;
    if((rc = mdb_dbi_open(txn, "path_hierarchy", flags|MDB_DUPSORT, &d->dbi_path_hierarchy))) return rc;
    return 0;
}

static int open_content_dbs(MDB_txn* txn, DbImpl* d, BOOL create){
    unsigned int flags = create? MDB_CREATE:0;
    int rc;
    if((rc = mdb_dbi_open(txn, "trigram_index", flags|MDB_DUPSORT, &d->dbi_trigram_index))) return rc;
    if((rc = mdb_dbi_open(txn, "string_meta", flags|MDB_CREATE, &d->dbi_string_meta))) return rc;
    if((rc = mdb_dbi_open(txn, "content_index", flags|MDB_DUPSORT, &d->dbi_content_index))) return rc;
    if((rc = mdb_dbi_open(txn, "author_index", flags|MDB_DUPSORT, &d->dbi_author_index))) return rc;
    if((rc = mdb_dbi_open(txn, "camera_index", flags|MDB_DUPSORT, &d->dbi_camera_index))) return rc;
    if((rc = mdb_dbi_open(txn, "lens_index", flags|MDB_DUPSORT, &d->dbi_lens_index))) return rc;
    if((rc = mdb_dbi_open(txn, "artist_index", flags|MDB_DUPSORT, &d->dbi_artist_index))) return rc;
    if((rc = mdb_dbi_open(txn, "album_index", flags|MDB_DUPSORT, &d->dbi_album_index))) return rc;
    if((rc = mdb_dbi_open(txn, "title_index", flags|MDB_DUPSORT, &d->dbi_title_index))) return rc;
    return 0;
}

static int ensure_indices(DbImpl* d, IndexLoadState desired){
    if(d->load_state >= desired) return 0;
    MDB_txn* txn;
    int rc = mdb_txn_begin(d->env, NULL, MDB_RDONLY, &txn);
    if(rc) return rc;
    if(d->load_state < INDEX_METADATA_LOADED && desired >= INDEX_METADATA_LOADED){
        rc = open_metadata_dbs(txn, d, FALSE);
        if(rc){ mdb_txn_abort(txn); return rc; }
        d->load_state = INDEX_METADATA_LOADED;
    }
    if(d->load_state < INDEX_CONTENT_LOADED && desired >= INDEX_CONTENT_LOADED){
        rc = open_content_dbs(txn, d, FALSE);
        if(rc){ mdb_txn_abort(txn); return rc; }
        d->load_state = INDEX_CONTENT_LOADED;
    }
    mdb_txn_abort(txn);
    return 0;
}

BOOL db_create(const wchar_t* path, size_t map_init_mb, size_t map_max_mb, Db** out_db){
    if(!out_db){ return FALSE; }
    *out_db=NULL;
    DbImpl* d = (DbImpl*)calloc(1,sizeof(DbImpl));
    if(!d){ return FALSE; }
    d->map_init = (size_t)map_init_mb * 1024ull * 1024ull;
    d->map_max  = (size_t)map_max_mb  * 1024ull * 1024ull;
    size_t min_init = 1024ull * 1024ull * 1024ull; // start with at least 1 GB
    if(d->map_init < min_init) d->map_init = min_init;
    if(d->map_max  < d->map_init)   d->map_max = d->map_init*4;
    ensure_dir(path);
    int rc = mdb_env_create(&d->env);
    if(rc){ set_mdb_error(d,rc); free(d); return FALSE; }
    mdb_env_set_mapsize(d->env, d->map_init);
    mdb_env_set_maxdbs(d->env, 64);
    char u8[MAX_PATH*3]; WideCharToMultiByte(CP_UTF8,0,path,-1,u8,sizeof(u8),NULL,NULL);
    rc = mdb_env_open(d->env, u8, MDB_WRITEMAP|MDB_MAPASYNC, 0664);
    if(rc){ set_mdb_error(d,rc); mdb_env_close(d->env); free(d); return FALSE; }
    wchar_t bloomPath[MAX_LONG_PATH];
    swprintf(bloomPath, MAX_LONG_PATH, L"%s\\bloom.dat", path);
    wchar_t lp[MAX_LONG_PATH]; make_long_path(bloomPath, lp, MAX_LONG_PATH);
    d->bloom_file = CreateFileW(lp, GENERIC_READ|GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if(d->bloom_file==INVALID_HANDLE_VALUE){ set_sys_error(d, GetLastError()); mdb_env_close(d->env); free(d); return FALSE; }
    LARGE_INTEGER sz; sz.QuadPart = 0; GetFileSizeEx(d->bloom_file, &sz); d->bloom_offset = sz.QuadPart; SetFilePointerEx(d->bloom_file, sz, NULL, FILE_BEGIN);
    MDB_txn* txn;
    if((rc = mdb_txn_begin(d->env, NULL, 0, &txn))){ set_mdb_error(d,rc); mdb_env_close(d->env); free(d); return FALSE; }
    if((rc = open_core_dbs(txn, d, TRUE))){ set_mdb_error(d,rc); mdb_txn_abort(txn); mdb_env_close(d->env); free(d); return FALSE; }
    if((rc = open_metadata_dbs(txn, d, TRUE))){ set_mdb_error(d,rc); mdb_txn_abort(txn); mdb_env_close(d->env); free(d); return FALSE; }
    if((rc = open_content_dbs(txn, d, TRUE))){ set_mdb_error(d,rc); mdb_txn_abort(txn); mdb_env_close(d->env); free(d); return FALSE; }
    // initialize header
    DbHeader hdr = {0};
    hdr.created_time = hdr.updated_time = now_filetime();
    hdr.map_size_bytes = d->map_init;
    hdr.generation = 0;
    hdr.journal_seq = 0;
    MDB_val k,v; const char* H="header"; to_mdb_val(H, strlen(H), &k); to_mdb_val(&hdr, sizeof(hdr), &v);
    if((rc = mdb_put(txn, d->dbi_meta, &k, &v, 0))){ set_mdb_error(d,rc); mdb_txn_abort(txn); mdb_env_close(d->env); free(d); return FALSE; }
    if((rc = mdb_txn_commit(txn))){ set_mdb_error(d,rc); mdb_env_close(d->env); free(d); return FALSE; }
    d->header_cache = hdr;
    d->load_state = INDEX_CONTENT_LOADED;
    *out_db = (Db*)d;
    return TRUE;
}

const DbHeader* db_open_readonly(const wchar_t* path, Db** out_db){
    if(!out_db){ return NULL; }
    *out_db=NULL;
    DbImpl* d = (DbImpl*)calloc(1,sizeof(DbImpl));
    if(!d){ return NULL; }
    d->map_init = 128*1024*1024; d->map_max = d->map_init*16;
    int rc = mdb_env_create(&d->env);
    if(rc){ set_mdb_error(d,rc); free(d); return NULL; }
    mdb_env_set_maxdbs(d->env, 64);
    char u8[MAX_PATH*3]; WideCharToMultiByte(CP_UTF8,0,path,-1,u8,sizeof(u8),NULL,NULL);
    if((rc = mdb_env_open(d->env, u8, MDB_RDONLY, 0664))){ set_mdb_error(d,rc); mdb_env_close(d->env); free(d); return NULL; }
    MDB_txn* txn;
    if((rc = mdb_txn_begin(d->env, NULL, MDB_RDONLY, &txn))){ set_mdb_error(d,rc); mdb_env_close(d->env); free(d); return NULL; }
    if((rc = open_core_dbs(txn, d, FALSE))){ set_mdb_error(d,rc); mdb_txn_abort(txn); mdb_env_close(d->env); free(d); return NULL; }
    // read header
    MDB_val k,v; const char* H="header"; to_mdb_val(H, strlen(H), &k);
    if((rc = mdb_get(txn, d->dbi_meta, &k, &v))){ set_mdb_error(d,rc); mdb_txn_abort(txn); mdb_env_close(d->env); free(d); return NULL; }
    if(v.mv_size < sizeof(DbHeader)){ mdb_txn_abort(txn); mdb_env_close(d->env); free(d); return NULL; }
    memcpy(&d->header_cache, v.mv_data, sizeof(DbHeader));
    mdb_txn_abort(txn);
    d->load_state = INDEX_CORE_LOADED;
    *out_db = (Db*)d;
    return &d->header_cache;
}

void db_close(Db* db_){
    if(!db_) return;
    DbImpl* d = (DbImpl*)db_;
    if(d->wtxn){ mdb_txn_abort(d->wtxn); d->wtxn=NULL; }
    if(d->env){ mdb_env_close(d->env); }
    if(d->bloom_file && d->bloom_file!=INVALID_HANDLE_VALUE){ CloseHandle(d->bloom_file); }
    free(d);
}

const DbHeader* db_header(Db* db_){
    DbImpl* d = (DbImpl*)db_;
    return &d->header_cache;
}
size_t db_current_mapsize(Db* db_){
    DbImpl* d = (DbImpl*)db_;
    MDB_envinfo info = {0};
    mdb_env_info(d->env, &info);
    return (size_t)info.me_mapsize;
}
size_t db_max_mapsize(Db* db_){
    DbImpl* d = (DbImpl*)db_;
    return d->map_max;
}
BOOL db_set_mapsize(Db* db_, size_t new_size_bytes){
    DbImpl* d = (DbImpl*)db_;
    if(new_size_bytes <= db_current_mapsize(db_) || new_size_bytes > d->map_max) return FALSE;
    int rc = mdb_env_set_mapsize(d->env, new_size_bytes);
    if(rc) set_mdb_error(d, rc); else set_error(d, DB_ERROR_NONE,0,NULL);
    if(rc==0){ d->header_cache.map_size_bytes = new_size_bytes; }
    return rc==0;
}
const DbError* db_last_error(Db* db_){
    DbImpl* d = (DbImpl*)db_;
    return &d->last_error;
}

BOOL db_ensure_loaded(Db* db_, IndexLoadState state){
    DbImpl* d = (DbImpl*)db_;
    int rc = ensure_indices(d, state);
    if(rc) set_mdb_error(d, rc); else set_error(d, DB_ERROR_NONE,0,NULL);
    return rc==0;
}

BOOL db_begin_write(Db* db_){
    DbImpl* d = (DbImpl*)db_;
    if(d->wtxn) return TRUE;
    int rc = ensure_indices(d, INDEX_CONTENT_LOADED);
    if(rc){ set_mdb_error(d, rc); return FALSE; }
    rc = mdb_txn_begin(d->env, NULL, 0, &d->wtxn);
    if(rc) set_mdb_error(d, rc); else set_error(d, DB_ERROR_NONE,0,NULL);
    return rc==0;
}
BOOL db_commit_write(Db* db_){
    DbImpl* d = (DbImpl*)db_;
    if(!d->wtxn) return TRUE;
    if(d->dirty){
        d->header_cache.generation++;
        d->header_cache.updated_time = now_filetime();
        MDB_val mk,mv; const char* H="header"; to_mdb_val(H, strlen(H), &mk);
        to_mdb_val(&d->header_cache, sizeof(d->header_cache), &mv);
        mdb_put(d->wtxn, d->dbi_meta, &mk, &mv, 0);
    }
    int rc = mdb_txn_commit(d->wtxn);
    if(rc) set_mdb_error(d, rc); else set_error(d, DB_ERROR_NONE,0,NULL);
    d->wtxn = NULL;
    if(rc==0){
        d->header_cache.map_size_bytes = db_current_mapsize(db_);
        mdb_env_sync(d->env, 1);
        d->dirty = FALSE;
    }
    return rc==0;
}
void db_abort_write(Db* db_){
    DbImpl* d = (DbImpl*)db_;
    if(d->wtxn){ mdb_txn_abort(d->wtxn); d->wtxn=NULL; }
}

BOOL db_compress(Db* db_, const wchar_t* out_path){
    if(!db_ || !out_path) return FALSE;
    DbImpl* d = (DbImpl*)db_;
    if(d->wtxn){
        if(!db_commit_write(db_)) return FALSE;
    }
    char u8[MAX_PATH*3];
    int rc = WideCharToMultiByte(CP_UTF8,0,out_path,-1,u8,sizeof(u8),NULL,NULL);
    if(rc<=0){ set_sys_error(d, GetLastError()); return FALSE; }
    rc = mdb_env_copy2(d->env, u8, MDB_CP_COMPACT);
    if(rc) set_mdb_error(d, rc); else set_error(d, DB_ERROR_NONE,0,NULL);
    return rc==0;
}

// String interning helpers
static BOOL str_by_id_with_retry(DbImpl* d, uint64_t id, MDB_val* out, int max_retries){
    MDB_val key; key.mv_data = &id; key.mv_size = sizeof(id);
    if(d->wtxn){
        return mdb_get(d->wtxn, d->dbi_strings, &key, out) == 0;
    }
    for(int i = 0; i < max_retries; ++i){
        MDB_txn* txn;
        if(mdb_txn_begin(d->env, NULL, MDB_RDONLY, &txn) == 0){
            int rc = mdb_get(txn, d->dbi_strings, &key, out);
            mdb_txn_abort(txn);
            if(rc == 0) return TRUE;
        }
        Sleep(1 << i);
    }
    return FALSE;
}

uint64_t db_intern_wstring(Db* db_, const wchar_t* s){
    if(!s || !s[0]) return 0;
    DbImpl* d = (DbImpl*)db_;
    int needed = WideCharToMultiByte(CP_UTF8,0,s,-1,NULL,0,NULL,NULL);
    if(needed<=0) return 0;
    char stack_u8[512];
    BOOL heap = needed > (int)sizeof(stack_u8);
    char* u8 = heap ? (char*)malloc(needed) : stack_u8;
    if(!u8) return 0;
    WideCharToMultiByte(CP_UTF8,0,s,-1,u8,needed,NULL,NULL);
    size_t u8len = (size_t)(needed-1);
    uint64_t h = hash64(u8, u8len);
    uint64_t cached = string_cache_lookup(u8, h);
    if(cached){ if(heap) free(u8); return cached; }
    // Try to read using the current write txn if present; otherwise open a RO txn.
    MDB_txn* rtxn = d->wtxn ? d->wtxn : NULL;
    BOOL need_abort = FALSE;
    if(!rtxn){
        if(mdb_txn_begin(d->env, NULL, MDB_RDONLY, &rtxn)!=0){ if(heap) free(u8); return 0; }
        need_abort = TRUE;
    }
    MDB_val k={.mv_data=u8,.mv_size=u8len}, v;
    int rc = mdb_get(rtxn, d->dbi_strrev, &k, &v);
    if(rc==0){
        uint64_t id = *(uint64_t*)v.mv_data;
        if(need_abort) mdb_txn_abort(rtxn);
        string_cache_insert(u8, h, id);
        if(heap) free(u8);
        return id;
    }
    if(need_abort) mdb_txn_abort(rtxn);
    if(!d->wtxn){ if(!db_begin_write(db_)){ if(heap) free(u8); return 0; } }
    uint64_t new_id = d->header_cache.string_count + 1;
    d->header_cache.string_count = new_id;
    MDB_val idkey={.mv_data=&new_id,.mv_size=sizeof(new_id)};
    MDB_val idval={.mv_data=u8,.mv_size=(size_t)(needed-1)};
    rc = mdb_put(d->wtxn, d->dbi_strings, &idkey, &idval, 0);
    if(rc){ set_mdb_error(d,rc); if(heap) free(u8); return 0; }
    MDB_val revval={.mv_data=&new_id,.mv_size=sizeof(new_id)};
    rc = mdb_put(d->wtxn, d->dbi_strrev, &k, &revval, 0);
    if(rc){ set_mdb_error(d,rc); if(heap) free(u8); return 0; }
    // update header
    MDB_val mk,mv; const char* H="header"; to_mdb_val(H, strlen(H), &mk);
    to_mdb_val(&d->header_cache, sizeof(d->header_cache), &mv);
    rc = mdb_put(d->wtxn, d->dbi_meta, &mk, &mv, 0);
    string_cache_insert(u8, h, new_id);
    if(heap) free(u8);
    if(rc){ set_mdb_error(d,rc); return 0; }
    return new_id;
}

// ---- Trigram helpers ----

static void extract_trigrams(const char* text, uint32_t** out_tris, size_t* out_count){
    // Limit the amount of data we look at to avoid reading past the end of a
    // malformed or extremely long string.  This mirrors the bloom filter limit
    // used elsewhere in the codebase.
    size_t len = strnlen(text, DB_BLOOM_MAX_BYTES);
    if(len < 3 || len == DB_BLOOM_MAX_BYTES){
        *out_tris = NULL;
        *out_count = 0;
        return;
    }

    size_t need = len - 2;
    static uint32_t* pool = NULL;
    static size_t pool_cap = 0;
    if(pool_cap < need){
        uint32_t* nb = (uint32_t*)realloc(pool, need * sizeof(uint32_t));
        if(!nb){ *out_tris = NULL; *out_count = 0; return; }
        pool = nb; pool_cap = need;
    }

#if defined(__SSE4_1__)
    size_t i = 0;
    if(len >= 16){
        size_t simd_end = len - 16;
        for(; i <= simd_end; i += 4){
            __m128i bytes = _mm_loadu_si128((const __m128i*)(text + i));
            __m128i b0 = _mm_cvtepu8_epi32(bytes);
            __m128i b1 = _mm_cvtepu8_epi32(_mm_srli_si128(bytes,1));
            __m128i b2 = _mm_cvtepu8_epi32(_mm_srli_si128(bytes,2));
            __m128i t  = _mm_or_si128(_mm_slli_epi32(b0,16),
                              _mm_or_si128(_mm_slli_epi32(b1,8), b2));
            _mm_storeu_si128((__m128i*)(pool + i), t);
        }
    }
    for(; i < need; ++i){
        pool[i] = ((uint8_t)text[i]<<16)|((uint8_t)text[i+1]<<8)|((uint8_t)text[i+2]);
    }
#else
    for(size_t i=0;i<need;i++){
        pool[i]=((uint8_t)text[i]<<16)|((uint8_t)text[i+1]<<8)|((uint8_t)text[i+2]);
    }
#endif
    *out_tris = pool;
    *out_count = need;
}

static void emit_trigrams(DbImpl* d, const char* name_u8, uint64_t name_id){
    size_t len = strlen(name_u8);
    if(len < 3) return;
    char stack_tmp[512];
    BOOL heap = len + 1 > sizeof(stack_tmp);
    char* tmp = heap ? (char*)malloc(len + 1) : stack_tmp;
    if(!tmp) return;
    memcpy(tmp, name_u8, len + 1);
    lowercase_ascii(tmp, len);

    uint32_t* tris; size_t tri_n;
    extract_trigrams(tmp, &tris, &tri_n);

    // Deduplicate trigrams to avoid duplicate index entries
    uint32_t seen[256]; UINT seen_n=0;
    for(size_t i=0;i<tri_n;i++){
        uint32_t key = tris[i];
        BOOL dup=FALSE;
        for(UINT j=0;j<seen_n;j++){ if(seen[j]==key){ dup=TRUE; break; } }
        if(dup) continue;
        if(seen_n<256) seen[seen_n++]=key;

        MDB_val k={.mv_data=&key,.mv_size=3}, v={.mv_data=&name_id,.mv_size=sizeof(name_id)};
        int rc = mdb_put(d->wtxn, d->dbi_trigram_index, &k, &v, MDB_NODUPDATA);
        if(rc && rc!=MDB_KEYEXIST){ set_mdb_error(d, rc); }
    }
    if(heap) free(tmp);
}

static void remove_trigrams(DbImpl* d, const char* name_u8, uint64_t name_id){
    size_t len = strlen(name_u8);
    if(len < 3) return;
    char stack_tmp[512];
    BOOL heap = len + 1 > sizeof(stack_tmp);
    char* tmp = heap ? (char*)malloc(len + 1) : stack_tmp;
    if(!tmp) return;
    memcpy(tmp, name_u8, len + 1);
    lowercase_ascii(tmp, len);

    uint32_t* tris; size_t tri_n;
    extract_trigrams(tmp, &tris, &tri_n);

    uint32_t seen[256]; UINT seen_n=0;
    for(size_t i=0;i<tri_n;i++){
        uint32_t key = tris[i];
        BOOL dup=FALSE;
        for(UINT j=0;j<seen_n;j++){ if(seen[j]==key){ dup=TRUE; break; } }
        if(dup) continue;
        if(seen_n<256) seen[seen_n++]=key;
        MDB_val k={.mv_data=&key,.mv_size=3}, v={.mv_data=&name_id,.mv_size=sizeof(name_id)};
        int rc = mdb_del(d->wtxn, d->dbi_trigram_index, &k, &v);
        if(rc && rc!=MDB_NOTFOUND){ set_mdb_error(d, rc); }
    }
    if(heap) free(tmp);
}

BOOL db_get_compressed_trigram(Db* db_, uint32_t trigram, CompressedTrigram* out){
    if(!db_ || !out) return FALSE;
    DbImpl* d = (DbImpl*)db_;
    if(!db_ensure_loaded(db_, INDEX_CONTENT_LOADED)) return FALSE;
    MDB_txn* txn; if(mdb_txn_begin(d->env, NULL, MDB_RDONLY, &txn)!=0) return FALSE;
    MDB_cursor* c=NULL; if(mdb_cursor_open(txn, d->dbi_trigram_index, &c)!=0){ mdb_txn_abort(txn); return FALSE; }
    uint8_t key_bytes[3]={ (uint8_t)(trigram>>16), (uint8_t)(trigram>>8), (uint8_t)trigram };
    MDB_val k={.mv_data=key_bytes,.mv_size=3}, v;
    uint32_t cap=0, n=0; uint32_t* buf=NULL; uint64_t prev=0;
    if(mdb_cursor_get(c,&k,&v,MDB_SET_KEY)==0){
        do{
            if(n==cap){
                uint32_t newcap = cap?cap*2:16;
                uint32_t* nb = (uint32_t*)realloc(buf, newcap*sizeof(uint32_t));
                if(!nb){ free(buf); mdb_cursor_close(c); mdb_txn_abort(txn); return FALSE; }
                buf = nb; cap = newcap;
            }
            uint64_t id = *(uint64_t*)v.mv_data;
            buf[n++] = (uint32_t)(id - prev);
            prev = id;
        }while(mdb_cursor_get(c,&k,&v,MDB_NEXT_DUP)==0);
    }
    mdb_cursor_close(c); mdb_txn_abort(txn);
    out->trigram.bytes[0]=key_bytes[0];
    out->trigram.bytes[1]=key_bytes[1];
    out->trigram.bytes[2]=key_bytes[2];
    out->string_id_count=n;
    out->string_ids=buf;
    return TRUE;
}

void db_free_compressed_trigram(CompressedTrigram* ct){
    if(ct && ct->string_ids){ free(ct->string_ids); ct->string_ids=NULL; ct->string_id_count=0; }
}



BOOL db_get_index_state(Db* db_, IndexState* out){
    DbImpl* d = (DbImpl*)db_;
    MDB_txn* txn; if(mdb_txn_begin(d->env,NULL,MDB_RDONLY,&txn)!=0) return FALSE;
    MDB_val k={.mv_data="index_state",.mv_size=11}, v;
    int rc = mdb_get(txn, d->dbi_meta, &k, &v);
    if(rc==0 && v.mv_size==sizeof(IndexState)){ memcpy(out, v.mv_data, sizeof(IndexState)); mdb_txn_abort(txn); return TRUE; }
    mdb_txn_abort(txn); return FALSE;
}
BOOL db_set_index_state(Db* db_, const IndexState* st){
    DbImpl* d = (DbImpl*)db_;
    if(!d->wtxn){ if(!db_begin_write(db_)) return FALSE; }
    MDB_val k={.mv_data="index_state",.mv_size=11}, v={.mv_data=(void*)st,.mv_size=sizeof(IndexState)};
    int rc = mdb_put(d->wtxn, d->dbi_meta, &k, &v, 0);
    if(rc) set_mdb_error(d, rc); else set_error(d, DB_ERROR_NONE,0,NULL);
    return rc==0;
}

// ---- Record write ----
BOOL db_put_records(Db* db_, const DbRecord* recs, size_t count){
    DbImpl* d = (DbImpl*)db_;
    if(!d->wtxn){ if(!db_begin_write(db_)) return FALSE; }
    d->dirty = TRUE;
    int rc = 0;
    for(size_t i=0;i<count;i++){
        const DbRecord* r = &recs[i];
        uint64_t id = d->header_cache.record_count + 1;
        d->header_cache.record_count = id;
        MDB_val k,v; to_mdb_val(&id, sizeof(id), &k); to_mdb_val((void*)r, sizeof(*r), &v);
        if((rc = mdb_put(d->wtxn, d->dbi_records, &k, &v, 0))){ set_mdb_error(d,rc); return FALSE; }
        // filename_index
        MDB_val ik,iv; to_mdb_val(&r->name_str_id, sizeof(r->name_str_id), &ik); to_mdb_val(&id, sizeof(id), &iv);
        rc = mdb_put(d->wtxn, d->dbi_fname_index, &ik, &iv, MDB_NODUPDATA);
        if(rc && rc!=MDB_KEYEXIST){ set_mdb_error(d,rc); return FALSE; }
        // parent_index & path_hierarchy
        MDB_val pk,pv; to_mdb_val(&r->parent_str_id, sizeof(r->parent_str_id), &pk); to_mdb_val(&id, sizeof(id), &pv);
        rc = mdb_put(d->wtxn, d->dbi_parent_index, &pk, &pv, MDB_NODUPDATA);
        if(rc && rc!=MDB_KEYEXIST){ set_mdb_error(d,rc); return FALSE; }
        rc = mdb_put(d->wtxn, d->dbi_path_hierarchy, &pk, &pv, MDB_NODUPDATA);
        if(rc && rc!=MDB_KEYEXIST){ set_mdb_error(d,rc); return FALSE; }
        // size_index (files only)
        if(r->type == DB_REC_FILE){
            MDB_val sk,sv; to_mdb_val(&r->file_size, sizeof(r->file_size), &sk); to_mdb_val(&id, sizeof(id), &sv);
            rc = mdb_put(d->wtxn, d->dbi_size_index, &sk, &sv, MDB_NODUPDATA);
            if(rc && rc!=MDB_KEYEXIST){ set_mdb_error(d,rc); return FALSE; }
        }
        // date_index (modified time day)
        uint64_t day = filetime_days(r->modified_time);
        MDB_val dk,dv; to_mdb_val(&day, sizeof(day), &dk); to_mdb_val(&id, sizeof(id), &dv);
        rc = mdb_put(d->wtxn, d->dbi_date_index, &dk, &dv, MDB_NODUPDATA);
        if(rc && rc!=MDB_KEYEXIST){ set_mdb_error(d,rc); return FALSE; }
        // mtime_index
        MDB_val mk,mv; to_mdb_val(&r->modified_time, sizeof(r->modified_time), &mk); to_mdb_val(&id, sizeof(id), &mv);
        rc = mdb_put(d->wtxn, d->dbi_mtime_index, &mk, &mv, MDB_NODUPDATA);
        if(rc && rc!=MDB_KEYEXIST){ set_mdb_error(d,rc); return FALSE; }
        // attributes index
        MDB_val ak,av; to_mdb_val(&r->attributes, sizeof(r->attributes), &ak); to_mdb_val(&id, sizeof(id), &av);
        rc = mdb_put(d->wtxn, d->dbi_attr_index, &ak, &av, MDB_NODUPDATA);
        if(rc && rc!=MDB_KEYEXIST){ set_mdb_error(d,rc); return FALSE; }
        // extension_index & trigrams from name
        // Fetch UTF-8 name by id
        MDB_val namev;
        if(str_by_id_with_retry(d, r->name_str_id, &namev, 5)){
            // ensure bloom meta exists
            MDB_val mk={.mv_data=&r->name_str_id,.mv_size=sizeof(r->name_str_id)}, mv;
            if(mdb_get(d->wtxn, d->dbi_string_meta, &mk, &mv)!=0){
                uint8_t bloom[8192];
                uint32_t tc = build_bloom_for_name((const char*)namev.mv_data, bloom);
                uint64_t off = d->bloom_offset;
                if(off > UINT64_MAX - 8192){ set_error(d, DB_ERROR_OS, 0, "bloom file too large"); return FALSE; }
                DWORD wr=0;
                if(!WriteFile(d->bloom_file, bloom, 8192, &wr, NULL) || wr != 8192){ set_sys_error(d, GetLastError()); return FALSE; }
                d->bloom_offset += wr;
                StringMeta sm={.trigram_count=tc,.bloom_offset=off};
                MDB_val smv={.mv_data=&sm,.mv_size=sizeof(sm)};
                mdb_put(d->wtxn, d->dbi_string_meta, &mk, &smv, 0);
            }
            char ext[32]; split_extension_utf8((const char*)namev.mv_data, ext, sizeof(ext));
            if(ext[0]){
                MDB_val ek={.mv_data=ext,.mv_size=strlen(ext)}, ev={.mv_data=&id,.mv_size=sizeof(id)};
                rc = mdb_put(d->wtxn, d->dbi_extension_index, &ek, &ev, MDB_NODUPDATA);
                if(rc && rc!=MDB_KEYEXIST){ set_mdb_error(d,rc); return FALSE; }
            }
            emit_trigrams(d, (const char*)namev.mv_data, r->name_str_id);
        }
        if(r->content_str_id){
            MDB_val ck, cv; to_mdb_val(&r->content_str_id, sizeof(r->content_str_id), &ck); to_mdb_val(&id, sizeof(id), &cv);
            rc = mdb_put(d->wtxn, d->dbi_content_index, &ck, &cv, MDB_NODUPDATA);
            if(rc && rc!=MDB_KEYEXIST){ set_mdb_error(d,rc); return FALSE; }
            MDB_val cvstr;
            if(str_by_id_with_retry(d, r->content_str_id, &cvstr, 5)){
                MDB_val mk={.mv_data=&r->content_str_id,.mv_size=sizeof(r->content_str_id)}, mv;
                if(mdb_get(d->wtxn, d->dbi_string_meta, &mk, &mv)!=0){
                    uint8_t bloom[8192];
                    uint32_t tc = build_bloom_for_name((const char*)cvstr.mv_data, bloom);
                    uint64_t off = d->bloom_offset;
                    if(off > UINT64_MAX - 8192){ set_error(d, DB_ERROR_OS, 0, "bloom file too large"); return FALSE; }
                    DWORD wr=0;
                    if(!WriteFile(d->bloom_file, bloom, 8192, &wr, NULL) || wr != 8192){ set_sys_error(d, GetLastError()); return FALSE; }
                    d->bloom_offset += wr;
                    StringMeta sm={.trigram_count=tc,.bloom_offset=off};
                    MDB_val smv={.mv_data=&sm,.mv_size=sizeof(sm)};
                    mdb_put(d->wtxn, d->dbi_string_meta, &mk, &smv, 0);
                }
                emit_trigrams(d, (const char*)cvstr.mv_data, r->content_str_id);
            }
        }
        if(r->author_str_id){
            MDB_val ak,av; to_mdb_val(&r->author_str_id, sizeof(r->author_str_id), &ak); to_mdb_val(&id, sizeof(id), &av);
            rc = mdb_put(d->wtxn, d->dbi_author_index, &ak, &av, MDB_NODUPDATA);
            if(rc && rc!=MDB_KEYEXIST){ set_mdb_error(d,rc); return FALSE; }
        }
        if(r->camera_str_id){
            MDB_val ck,av; to_mdb_val(&r->camera_str_id, sizeof(r->camera_str_id), &ck); to_mdb_val(&id, sizeof(id), &av);
            rc = mdb_put(d->wtxn, d->dbi_camera_index, &ck, &av, MDB_NODUPDATA);
            if(rc && rc!=MDB_KEYEXIST){ set_mdb_error(d,rc); return FALSE; }
        }
        if(r->lens_str_id){
            MDB_val lk,av; to_mdb_val(&r->lens_str_id, sizeof(r->lens_str_id), &lk); to_mdb_val(&id, sizeof(id), &av);
            rc = mdb_put(d->wtxn, d->dbi_lens_index, &lk, &av, MDB_NODUPDATA);
            if(rc && rc!=MDB_KEYEXIST){ set_mdb_error(d,rc); return FALSE; }
        }
        if(r->artist_str_id){
            MDB_val ark,av; to_mdb_val(&r->artist_str_id, sizeof(r->artist_str_id), &ark); to_mdb_val(&id, sizeof(id), &av);
            rc = mdb_put(d->wtxn, d->dbi_artist_index, &ark, &av, MDB_NODUPDATA);
            if(rc && rc!=MDB_KEYEXIST){ set_mdb_error(d,rc); return FALSE; }
        }
        if(r->album_str_id){
            MDB_val abk,av; to_mdb_val(&r->album_str_id, sizeof(r->album_str_id), &abk); to_mdb_val(&id, sizeof(id), &av);
            rc = mdb_put(d->wtxn, d->dbi_album_index, &abk, &av, MDB_NODUPDATA);
            if(rc && rc!=MDB_KEYEXIST){ set_mdb_error(d,rc); return FALSE; }
        }
        if(r->title_str_id){
            MDB_val tk,av; to_mdb_val(&r->title_str_id, sizeof(r->title_str_id), &tk); to_mdb_val(&id, sizeof(id), &av);
            rc = mdb_put(d->wtxn, d->dbi_title_index, &tk, &av, MDB_NODUPDATA);
            if(rc && rc!=MDB_KEYEXIST){ set_mdb_error(d,rc); return FALSE; }
        }
    }
    // update header
    MDB_val mk,mv; const char* H="header"; to_mdb_val(H, strlen(H), &mk);
    to_mdb_val(&d->header_cache, sizeof(d->header_cache), &mv);
    rc = mdb_put(d->wtxn, d->dbi_meta, &mk, &mv, 0);
    if(rc) set_mdb_error(d, rc); else set_error(d, DB_ERROR_NONE,0,NULL);
    return rc==0;
}

static BOOL db_delete_record(DbImpl* d, uint64_t id, const DbRecord* r){
    MDB_val k;
    to_mdb_val(&id, sizeof(id), &k);
    mdb_del(d->wtxn, d->dbi_records, &k, NULL);

    MDB_val ik,iv; to_mdb_val(&r->name_str_id, sizeof(r->name_str_id), &ik); to_mdb_val(&id, sizeof(id), &iv);
    mdb_del(d->wtxn, d->dbi_fname_index, &ik, &iv);

    MDB_val pk,pv; to_mdb_val(&r->parent_str_id, sizeof(r->parent_str_id), &pk); to_mdb_val(&id, sizeof(id), &pv);
    mdb_del(d->wtxn, d->dbi_parent_index, &pk, &pv);
    mdb_del(d->wtxn, d->dbi_path_hierarchy, &pk, &pv);

    if(r->type == DB_REC_FILE){
        MDB_val sk,sv; to_mdb_val(&r->file_size, sizeof(r->file_size), &sk); to_mdb_val(&id, sizeof(id), &sv);
        mdb_del(d->wtxn, d->dbi_size_index, &sk, &sv);
    }

    uint64_t day = filetime_days(r->modified_time);
    MDB_val dk,dv; to_mdb_val(&day, sizeof(day), &dk); to_mdb_val(&id, sizeof(id), &dv);
    mdb_del(d->wtxn, d->dbi_date_index, &dk, &dv);
    MDB_val mk,mv; to_mdb_val(&r->modified_time, sizeof(r->modified_time), &mk); to_mdb_val(&id, sizeof(id), &mv);
    mdb_del(d->wtxn, d->dbi_mtime_index, &mk, &mv);
    MDB_val ak,av; to_mdb_val(&r->attributes, sizeof(r->attributes), &ak); to_mdb_val(&id, sizeof(id), &av);
    mdb_del(d->wtxn, d->dbi_attr_index, &ak, &av);

    MDB_val namev;
    if(str_by_id_with_retry(d, r->name_str_id, &namev, 5)){
        char ext[32]; split_extension_utf8((const char*)namev.mv_data, ext, sizeof(ext));
        if(ext[0]){
            MDB_val ek={.mv_data=ext,.mv_size=strlen(ext)}, ev={.mv_data=&id,.mv_size=sizeof(id)};
            mdb_del(d->wtxn, d->dbi_extension_index, &ek, &ev);
        }
        remove_trigrams(d, (const char*)namev.mv_data, r->name_str_id);
    }

    if(r->content_str_id){
        MDB_val ck,cv; to_mdb_val(&r->content_str_id, sizeof(r->content_str_id), &ck); to_mdb_val(&id, sizeof(id), &cv);
        mdb_del(d->wtxn, d->dbi_content_index, &ck, &cv);
        MDB_val cvstr;
        if(str_by_id_with_retry(d, r->content_str_id, &cvstr, 5)){
            remove_trigrams(d, (const char*)cvstr.mv_data, r->content_str_id);
        }
    }

    if(r->author_str_id){ MDB_val ak,av; to_mdb_val(&r->author_str_id, sizeof(r->author_str_id), &ak); to_mdb_val(&id, sizeof(id), &av); mdb_del(d->wtxn, d->dbi_author_index, &ak, &av); }
    if(r->camera_str_id){ MDB_val ck,av; to_mdb_val(&r->camera_str_id, sizeof(r->camera_str_id), &ck); to_mdb_val(&id, sizeof(id), &av); mdb_del(d->wtxn, d->dbi_camera_index, &ck, &av); }
    if(r->lens_str_id){ MDB_val lk,av; to_mdb_val(&r->lens_str_id, sizeof(r->lens_str_id), &lk); to_mdb_val(&id, sizeof(id), &av); mdb_del(d->wtxn, d->dbi_lens_index, &lk, &av); }
    if(r->artist_str_id){ MDB_val ark,av; to_mdb_val(&r->artist_str_id, sizeof(r->artist_str_id), &ark); to_mdb_val(&id, sizeof(id), &av); mdb_del(d->wtxn, d->dbi_artist_index, &ark, &av); }
    if(r->album_str_id){ MDB_val abk,av; to_mdb_val(&r->album_str_id, sizeof(r->album_str_id), &abk); to_mdb_val(&id, sizeof(id), &av); mdb_del(d->wtxn, d->dbi_album_index, &abk, &av); }
    if(r->title_str_id){ MDB_val tk,av; to_mdb_val(&r->title_str_id, sizeof(r->title_str_id), &tk); to_mdb_val(&id, sizeof(id), &av); mdb_del(d->wtxn, d->dbi_title_index, &tk, &av); }
    return TRUE;
}

BOOL db_delete_path(Db* db_, const wchar_t* parent, const wchar_t* name){
    DbImpl* d = (DbImpl*)db_;
    if(!d->wtxn){ if(!db_begin_write(db_)) return FALSE; }
    d->dirty = TRUE;
    uint64_t parent_id = db_intern_wstring(db_, parent);
    uint64_t name_id   = db_intern_wstring(db_, name);
    if(!parent_id || !name_id) return TRUE;
    MDB_cursor* cur;
    MDB_val key={.mv_data=&name_id,.mv_size=sizeof(name_id)}, val;
    int rc = mdb_cursor_open(d->wtxn, d->dbi_fname_index, &cur);
    if(rc){ set_mdb_error(d, rc); return FALSE; }
    rc = mdb_cursor_get(cur, &key, &val, MDB_SET);
    while(rc==0){
        uint64_t id = *(uint64_t*)val.mv_data;
        MDB_val rk,rv; to_mdb_val(&id,sizeof(id),&rk);
        if(mdb_get(d->wtxn, d->dbi_records, &rk, &rv)==0){
            DbRecord r; memcpy(&r, rv.mv_data, sizeof(r));
            if(r.parent_str_id == parent_id){
                db_delete_record(d, id, &r);
                break;
            }
        }
        rc = mdb_cursor_get(cur, &key, &val, MDB_NEXT_DUP);
    }
    mdb_cursor_close(cur);
    return TRUE;
}

BOOL db_get_record_by_path(Db* db_, const wchar_t* parent, const wchar_t* name, DbRecord* out){
    DbImpl* d = (DbImpl*)db_;
    MDB_txn* txn = d->wtxn;
    BOOL own_txn = FALSE;
    if(!txn){
        if(mdb_txn_begin(d->env, NULL, MDB_RDONLY, &txn)!=0) return FALSE;
        own_txn = TRUE;
    }
    uint64_t parent_id = db_intern_wstring(db_, parent);
    uint64_t name_id   = db_intern_wstring(db_, name);
    if(!parent_id || !name_id){ if(own_txn) mdb_txn_abort(txn); return FALSE; }
    MDB_cursor* cur;
    MDB_val key={.mv_data=&name_id,.mv_size=sizeof(name_id)}, val;
    int rc = mdb_cursor_open(txn, d->dbi_fname_index, &cur);
    if(rc){ if(own_txn) mdb_txn_abort(txn); return FALSE; }
    rc = mdb_cursor_get(cur, &key, &val, MDB_SET);
    BOOL found = FALSE;
    while(rc==0){
        uint64_t id = *(uint64_t*)val.mv_data;
        MDB_val rk,rv; to_mdb_val(&id,sizeof(id),&rk);
        if(mdb_get(txn, d->dbi_records, &rk, &rv)==0){
            DbRecord* r=(DbRecord*)rv.mv_data;
            if(r->parent_str_id == parent_id){
                if(out) *out = *r;
                found = TRUE;
                break;
            }
        }
        rc = mdb_cursor_get(cur, &key, &val, MDB_NEXT_DUP);
    }
    mdb_cursor_close(cur);
    if(own_txn) mdb_txn_abort(txn);
    return found;
}
