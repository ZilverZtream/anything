#ifndef DATABASE_H
#define DATABASE_H

#include <windows.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- LMDB forward decl ----
typedef struct MDB_env MDB_env;
typedef unsigned int MDB_dbi;
typedef struct MDB_txn MDB_txn;

// Suggested batch size for the writer.
#ifndef LMDB_BATCH_SIZE_SUGGESTION
#define LMDB_BATCH_SIZE_SUGGESTION 50000
#endif

// Record type for items written into the DB.
typedef enum DbRecordType {
    DB_REC_FILE = 1,
    DB_REC_DIR  = 2
} DbRecordType;

// Persisted record stored in "records" database.
// String values are interned into the "strings" DB and referred to by 32-bit ids.
#pragma pack(push, 1)
typedef struct DbRecord {
    uint8_t   type;            // DbRecordType
    uint32_t  parent_str_id;   // interned parent directory full path (UTF-8)
    uint32_t  name_str_id;     // interned item name (UTF-8)
    uint64_t  file_size;       // for files only (0 for directories)
    uint64_t  creation_time;   // FILETIME as 64-bit
    uint64_t  modified_time;   // FILETIME as 64-bit
    uint64_t  access_time;     // FILETIME as 64-bit
    uint32_t  attributes;      // FILE_ATTRIBUTE_xxx
} DbRecord;
#pragma pack(pop)

// Database header saved under "meta" database.
typedef struct DbHeader {
    uint32_t version;          // schema version
    uint32_t reserved;
    uint64_t created_utc;      // time(NULL) at creation
    uint64_t items_files;
    uint64_t items_dirs;
    uint64_t map_size_bytes;
} DbHeader;

typedef struct Db Db;

// Create (or open) a writable database environment at directory `dir`.
// - map_init_mb: initial mapsize in megabytes (if 0, a sensible default is used)
// - map_max_mb : maximum mapsize allowed to grow to (if 0, default 4096 MB)
// Returns TRUE on success. On success, *out_db is valid and writable.
BOOL db_create(const WCHAR* dir, size_t map_init_mb, size_t map_max_mb, Db** out_db);

// Open a read-only handle and return a pointer to the header (valid for the handle lifetime).
// NOTE: The returned Db* is read-only. Close with db_close().
const DbHeader* db_open_readonly(const WCHAR* dir, Db** out_db);

// Close the database (both read-only and read-write).
void db_close(Db* db);

// Transaction helpers (writer thread).
BOOL db_begin_write(Db* db);
BOOL db_commit_write(Db* db);
void db_abort_write(Db* db);

// Read transaction helpers (for printing, etc.)
BOOL db_begin_read(Db* db);
void db_end_read(Db* db);

// Access header (valid after open/create; in read txns also).
const DbHeader* db_header(Db* db);

// String interning utilities (UTF-8 / UTF-16)
// Returns interned string id; never 0.
uint32_t db_intern_string(Db* db, const char* utf8, size_t len);
uint32_t db_intern_wstring(Db* db, const WCHAR* w);

// Fetch a string by id within an active READ transaction.
// The pointer is valid until db_end_read() for the current txn is called.
const char* db_get_string_in_txn(Db* db, uint32_t id, size_t* out_len);

// Put a single record (must be in an active write txn).
BOOL db_put_record(Db* db, const DbRecord* rec);

// Put a batch of records in the current write txn (caller may retry on failure).
BOOL db_put_records(Db* db, const DbRecord* recs, size_t count);

// Map size management (called when MDB_MAP_FULL is hit).
size_t db_current_mapsize(Db* db);
size_t db_max_mapsize(Db* db);
BOOL db_set_mapsize(Db* db, size_t new_size_bytes); // enlarge only

// Last LMDB error code encountered (for diagnostics).
int db_last_error(Db* db);

#ifdef __cplusplus
}
#endif

#endif // DATABASE_H
