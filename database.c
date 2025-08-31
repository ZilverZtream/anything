// database.c - LMDB wrapper for ANYTHING indexer
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>
#include "database.h"

// You must have LMDB headers/libraries available in your include/lib paths.
#include "lmdb.h"

// --- Helpers ---------------------------------------------------------------

static inline uint64_t filetime_now_utc() {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

static inline void* xmalloc(size_t n) {
    void* p = malloc(n);
    if (!p) { fprintf(stderr, "Out of memory (%zu bytes)\n", n); ExitProcess(3); }
    return p;
}

// --- Db handle -------------------------------------------------------------

struct Db {
    MDB_env* env;
    MDB_dbi  dbi_meta;
    MDB_dbi  dbi_strings;    // id -> bytes
    MDB_dbi  dbi_rstrings;   // bytes -> id  (dups disallowed)
    MDB_dbi  dbi_records;    // rec_id (u64) -> DbRecord
    MDB_dbi  dbi_fname_idx;  // name_str_id (u32) -> rec_id (u64), dupsort (many)
    MDB_txn* wtxn;           // current write txn (writer thread only)
    MDB_txn* rtxn;           // current read txn (for queries/printing)
    DbHeader header;
    uint64_t next_rec_id;
    uint32_t next_str_id;
    size_t   map_max_bytes;
    int      last_err;
    BOOL     readonly;
};

static const uint32_t DB_SCHEMA_VERSION = 1;

// --- Encoding helpers (little endian) -------------------------------------

static void put_u32(uint8_t* b, uint32_t v) {
    b[0]=(uint8_t)v; b[1]=(uint8_t)(v>>8); b[2]=(uint8_t)(v>>16); b[3]=(uint8_t)(v>>24);
}
static void put_u64(uint8_t* b, uint64_t v) {
    for (int i=0;i<8;i++) b[i]=(uint8_t)(v>>(i*8));
}
static uint32_t read_u32(const uint8_t* b) {
    return (uint32_t)b[0] | ((uint32_t)b[1]<<8) | ((uint32_t)b[2]<<16) | ((uint32_t)b[3]<<24);
}
static uint64_t read_u64(const uint8_t* b) {
    uint64_t v=0; for (int i=0;i<8;i++) v |= ((uint64_t)b[i]) << (i*8); return v;
}

// --- Internal: write meta header ------------------------------------------

static void db_write_header(Db* db, MDB_txn* txn) {
    uint8_t key_b[6] = { 'h','e','a','d','e','r' };
    MDB_val key = { sizeof(key_b), key_b };
    MDB_val val = { sizeof(db->header), (void*)&db->header };
    int rc = mdb_put(txn, db->dbi_meta, &key, &val, 0);
    if (rc) db->last_err = rc;
}

static BOOL db_read_header(Db* db, MDB_txn* txn) {
    uint8_t key_b[6] = { 'h','e','a','d','e','r' };
    MDB_val key = { sizeof(key_b), key_b };
    MDB_val val = {0};
    int rc = mdb_get(txn, db->dbi_meta, &key, &val);
    if (rc) { db->last_err = rc; return FALSE; }
    if (val.mv_size != sizeof(DbHeader)) return FALSE;
    memcpy(&db->header, val.mv_data, sizeof(DbHeader));
    return TRUE;
}

static void db_write_counters(Db* db, MDB_txn* txn) {
    uint8_t k1[6] = { 'n','r','e','c','i','d' };
    uint8_t k2[6] = { 'n','s','t','r','i','d' };
    uint8_t b1[8], b2[4];
    put_u64(b1, db->next_rec_id);
    put_u32(b2, db->next_str_id);
    MDB_val key, val;
    key.mv_data = k1; key.mv_size = sizeof(k1);
    val.mv_data = b1; val.mv_size = sizeof(b1);
    int rc = mdb_put(txn, db->dbi_meta, &key, &val, 0);
    if (rc) db->last_err = rc;
    key.mv_data = k2; key.mv_size = sizeof(k2);
    val.mv_data = b2; val.mv_size = sizeof(b2);
    rc = mdb_put(txn, db->dbi_meta, &key, &val, 0);
    if (rc) db->last_err = rc;
}

static BOOL db_read_counters(Db* db, MDB_txn* txn) {
    uint8_t k1[6] = { 'n','r','e','c','i','d' };
    uint8_t k2[6] = { 'n','s','t','r','i','d' };
    MDB_val key, val;
    key.mv_data = k1; key.mv_size = sizeof(k1);
    int rc = mdb_get(txn, db->dbi_meta, &key, &val);
    if (rc == MDB_NOTFOUND) { db->next_rec_id = 1; } 
    else if (rc) { db->last_err = rc; return FALSE; }
    else { db->next_rec_id = read_u64((uint8_t*)val.mv_data); }

    key.mv_data = k2; key.mv_size = sizeof(k2);
    rc = mdb_get(txn, db->dbi_meta, &key, &val);
    if (rc == MDB_NOTFOUND) { db->next_str_id = 1; }
    else if (rc) { db->last_err = rc; return FALSE; }
    else { db->next_str_id = read_u32((uint8_t*)val.mv_data); }
    return TRUE;
}

// --- Public API ------------------------------------------------------------

static BOOL ensure_dir_exists(const WCHAR* path) {
    DWORD attr = GetFileAttributesW(path);
    if (attr != INVALID_FILE_ATTRIBUTES) {
        return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    return CreateDirectoryW(path, NULL);
}

BOOL db_create(const WCHAR* dir, size_t map_init_mb, size_t map_max_mb, Db** out_db) {
    if (!ensure_dir_exists(dir)) {
        fwprintf(stderr, L"[db] Failed to create directory: %ls\n", dir);
        return FALSE;
    }
    Db* db = (Db*)xmalloc(sizeof(Db));
    ZeroMemory(db, sizeof(*db));
    db->readonly = FALSE;

    size_t init_mb = (map_init_mb? map_init_mb : 256);
    size_t max_mb  = (map_max_mb?  map_max_mb  : 4096);
    db->map_max_bytes = max_mb * 1024ull * 1024ull;

    int rc = mdb_env_create(&db->env);
    if (rc) { db->last_err = rc; goto fail; }

    // allow sub-databases
    mdb_env_set_maxdbs(db->env, 8);
    mdb_env_set_mapsize(db->env, init_mb * 1024ull * 1024ull);

    // Open env (will create data.mdb/lock.mdb in dir)
    char path_utf8[MAX_PATH*4];
    WideCharToMultiByte(CP_UTF8,0,dir,-1,path_utf8, (int)sizeof(path_utf8),NULL,NULL);
    rc = mdb_env_open(db->env, path_utf8, 0, 0664);
    if (rc) { db->last_err = rc; goto fail; }

    MDB_txn* txn = NULL;
    rc = mdb_txn_begin(db->env, NULL, 0, &txn);
    if (rc) { db->last_err = rc; goto fail; }

    rc = mdb_dbi_open(txn, "meta", MDB_CREATE, &db->dbi_meta);
    if (rc) { db->last_err = rc; goto txfail; }
    rc = mdb_dbi_open(txn, "strings", MDB_CREATE, &db->dbi_strings);
    if (rc) { db->last_err = rc; goto txfail; }
    rc = mdb_dbi_open(txn, "rstrings", MDB_CREATE, &db->dbi_rstrings);
    if (rc) { db->last_err = rc; goto txfail; }
    rc = mdb_dbi_open(txn, "records", MDB_CREATE, &db->dbi_records);
    if (rc) { db->last_err = rc; goto txfail; }
    rc = mdb_dbi_open(txn, "filename_index", MDB_CREATE | MDB_DUPSORT, &db->dbi_fname_idx);
    if (rc) { db->last_err = rc; goto txfail; }

    // Initialize header and counters
    db->header.version = DB_SCHEMA_VERSION;
    db->header.reserved = 0;
    db->header.created_utc = (uint64_t)time(NULL);
    db->header.items_files = 0;
    db->header.items_dirs = 0;
    size_t mapsize_bytes=0;
    MDB_envinfo info; mdb_env_info(db->env, &info);
    mapsize_bytes = (size_t)info.me_mapsize;
    db->header.map_size_bytes = mapsize_bytes;

    db->next_rec_id = 1;
    db->next_str_id = 1;
    db_write_header(db, txn);
    db_write_counters(db, txn);

    rc = mdb_txn_commit(txn);
    if (rc) { db->last_err = rc; goto fail; }

    *out_db = db;
    return TRUE;

txfail:
    mdb_txn_abort(txn);
fail:
    if (db->env) mdb_env_close(db->env);
    free(db);
    return FALSE;
}

const DbHeader* db_open_readonly(const WCHAR* dir, Db** out_db) {
    Db* db = (Db*)xmalloc(sizeof(Db));
    ZeroMemory(db, sizeof(*db));
    db->readonly = TRUE;

    int rc = mdb_env_create(&db->env);
    if (rc) { db->last_err = rc; goto fail; }
    mdb_env_set_maxdbs(db->env, 8);

    char path_utf8[MAX_PATH*4];
    WideCharToMultiByte(CP_UTF8,0,dir,-1,path_utf8,(int)sizeof(path_utf8),NULL,NULL);
    rc = mdb_env_open(db->env, path_utf8, MDB_RDONLY, 0444);
    if (rc) { db->last_err = rc; goto fail; }

    MDB_txn* txn = NULL;
    rc = mdb_txn_begin(db->env, NULL, MDB_RDONLY, &txn);
    if (rc) { db->last_err = rc; goto fail; }

    rc = mdb_dbi_open(txn, "meta", 0, &db->dbi_meta);
    if (rc) { db->last_err = rc; goto txfail; }
    rc = mdb_dbi_open(txn, "strings", 0, &db->dbi_strings);
    if (rc) { db->last_err = rc; goto txfail; }
    rc = mdb_dbi_open(txn, "rstrings", 0, &db->dbi_rstrings);
    if (rc) { db->last_err = rc; goto txfail; }
    rc = mdb_dbi_open(txn, "records", 0, &db->dbi_records);
    if (rc) { db->last_err = rc; goto txfail; }
    rc = mdb_dbi_open(txn, "filename_index", 0, &db->dbi_fname_idx);
    if (rc) { db->last_err = rc; goto txfail; }

    if (!db_read_header(db, txn)) goto txfail;
    db_read_counters(db, txn); // not strictly needed read-only

    mdb_txn_commit(txn);
    *out_db = db;
    return &db->header;

txfail:
    mdb_txn_abort(txn);
fail:
    if (db->env) mdb_env_close(db->env);
    free(db);
    return NULL;
}

void db_close(Db* db) {
    if (!db) return;
    if (db->wtxn) { mdb_txn_abort(db->wtxn); db->wtxn=NULL; }
    if (db->rtxn) { mdb_txn_abort(db->rtxn); db->rtxn=NULL; }
    if (db->env) { mdb_env_close(db->env); db->env=NULL; }
    free(db);
}

BOOL db_begin_write(Db* db) {
    if (!db || db->readonly) return FALSE;
    if (db->wtxn) return TRUE;
    int rc = mdb_txn_begin(db->env, NULL, 0, &db->wtxn);
    db->last_err = rc;
    return rc==0;
}

BOOL db_commit_write(Db* db) {
    if (!db || !db->wtxn) return FALSE;
    db_write_header(db, db->wtxn);
    db_write_counters(db, db->wtxn);
    int rc = mdb_txn_commit(db->wtxn);
    db->wtxn = NULL;
    db->last_err = rc;
    return rc==0;
}

void db_abort_write(Db* db) {
    if (db && db->wtxn) {
        mdb_txn_abort(db->wtxn);
        db->wtxn=NULL;
    }
}

BOOL db_begin_read(Db* db) {
    if (!db) return FALSE;
    if (db->rtxn) return TRUE;
    int rc = mdb_txn_begin(db->env, NULL, MDB_RDONLY, &db->rtxn);
    db->last_err = rc;
    return rc==0;
}
void db_end_read(Db* db) {
    if (db && db->rtxn) {
        mdb_txn_abort(db->rtxn);
        db->rtxn = NULL;
    }
}

const DbHeader* db_header(Db* db) { return &db->header; }

static uint32_t rstrings_lookup_id(Db* db, MDB_txn* txn, const char* s, size_t len) {
    MDB_val key = { len, (void*)s };
    MDB_val val = {0};
    int rc = mdb_get(txn, db->dbi_rstrings, &key, &val);
    if (rc==0 && val.mv_size==4) {
        return read_u32((uint8_t*)val.mv_data);
    }
    return 0;
}

uint32_t db_intern_string(Db* db, const char* s, size_t len) {
    if (!db->wtxn) return 0;
    uint32_t id = rstrings_lookup_id(db, db->wtxn, s, len);
    if (id) return id;

    id = db->next_str_id++;

    // write strings: id -> bytes
    uint8_t kbuf[4]; put_u32(kbuf, id);
    MDB_val key = { sizeof(kbuf), kbuf };
    MDB_val val = { len, (void*)s };
    int rc = mdb_put(db->wtxn, db->dbi_strings, &key, &val, 0);
    if (rc) { db->last_err = rc; return 0; }

    // write rstrings: bytes -> id
    MDB_val rkey = { len, (void*)s };
    uint8_t vbuf[4]; put_u32(vbuf, id);
    MDB_val rval = { sizeof(vbuf), vbuf };
    rc = mdb_put(db->wtxn, db->dbi_rstrings, &rkey, &rval, 0);
    if (rc) { db->last_err = rc; return 0; }

    return id;
}

uint32_t db_intern_wstring(Db* db, const WCHAR* w) {
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char* buf = (char*)_malloca(len);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, len, NULL, NULL);
    uint32_t id = db_intern_string(db, buf, len-1);
    _freea(buf);
    return id;
}

const char* db_get_string_in_txn(Db* db, uint32_t id, size_t* out_len) {
    if (!db->rtxn) return NULL;
    uint8_t kbuf[4]; put_u32(kbuf, id);
    MDB_val key = { sizeof(kbuf), kbuf };
    MDB_val val = {0};
    int rc = mdb_get(db->rtxn, db->dbi_strings, &key, &val);
    if (rc) { db->last_err = rc; return NULL; }
    if (out_len) *out_len = val.mv_size;
    return (const char*)val.mv_data; // valid for txn lifetime
}

BOOL db_put_record(Db* db, const DbRecord* rec) {
    if (!db->wtxn) return FALSE;

    uint8_t keybuf[8];
    put_u64(keybuf, db->next_rec_id);
    MDB_val key = { sizeof(keybuf), keybuf };
    MDB_val val = { sizeof(DbRecord), (void*)rec };

    int rc = mdb_put(db->wtxn, db->dbi_records, &key, &val, 0);
    if (rc) { db->last_err = rc; return FALSE; }

    // filename index: name_str_id -> rec_id (dups ok)
    uint8_t nbuf[4]; put_u32(nbuf, rec->name_str_id);
    MDB_val nkey = { sizeof(nbuf), nbuf };
    MDB_val nval = { sizeof(keybuf), keybuf };
    rc = mdb_put(db->wtxn, db->dbi_fname_idx, &nkey, &nval, 0);
    if (rc) { db->last_err = rc; return FALSE; }

    db->header.map_size_bytes = db_current_mapsize(db);

    if (rec->type == DB_REC_FILE) db->header.items_files++;
    else if (rec->type == DB_REC_DIR) db->header.items_dirs++;

    db->next_rec_id++;
    return TRUE;
}

BOOL db_put_records(Db* db, const DbRecord* recs, size_t count) {
    for (size_t i=0;i<count;i++) {
        if (!db_put_record(db, &recs[i])) return FALSE;
    }
    return TRUE;
}

size_t db_current_mapsize(Db* db) {
    MDB_envinfo info;
    int rc = mdb_env_info(db->env, &info);
    if (rc) { db->last_err = rc; return 0; }
    return (size_t)info.me_mapsize;
}

size_t db_max_mapsize(Db* db) {
    return db->map_max_bytes;
}

BOOL db_set_mapsize(Db* db, size_t new_size_bytes) {
    size_t cur = db_current_mapsize(db);
    if (new_size_bytes <= cur) return TRUE;
    if (new_size_bytes > db->map_max_bytes) new_size_bytes = db->map_max_bytes;
    // Must not have an active write txn.
    if (db->wtxn) return FALSE;
    int rc = mdb_env_set_mapsize(db->env, new_size_bytes);
    db->last_err = rc;
    return rc==0;
}

int db_last_error(Db* db) { return db? db->last_err : 0; }
