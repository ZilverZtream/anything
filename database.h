#ifndef DATABASE_H
#define DATABASE_H
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <wchar.h>
#include "anything.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Db Db;
typedef struct DbRecord DbRecord;
typedef struct DbHeader DbHeader;
typedef struct IndexState {
    uint64_t     last_usn;
    uint64_t     last_scan_time;
    uint64_t     index_version;
    uint8_t      drive_signatures[26][32];
    IndexingLevel indexing_level;
} IndexState;

typedef enum {
    INDEX_CORE_LOADED,     // Names only
    INDEX_METADATA_LOADED, // + file attributes
    INDEX_CONTENT_LOADED   // + full text
} IndexLoadState;

typedef struct {
    uint8_t bytes[3];
} uint24_t;

typedef struct CompressedTrigram {
    uint24_t trigram;          // packed 24-bit trigram
    uint32_t string_id_count;  // number of encoded deltas
    uint32_t* string_ids;      // delta-encoded string IDs
} CompressedTrigram;

BOOL db_create(const wchar_t* path, size_t map_init_mb, size_t map_max_mb, Db** out_db);
const DbHeader* db_open_readonly(const wchar_t* path, Db** out_db);
void db_close(Db* db);

const DbHeader* db_header(Db* db);
size_t db_current_mapsize(Db* db);
size_t db_max_mapsize(Db* db);
BOOL   db_set_mapsize(Db* db, size_t new_size_bytes);
int    db_last_error(Db* db);

BOOL db_ensure_loaded(Db* db, IndexLoadState state);

BOOL db_begin_write(Db* db);
BOOL db_commit_write(Db* db);
void db_abort_write(Db* db);

BOOL db_get_index_state(Db* db, IndexState* out);
BOOL db_set_index_state(Db* db, const IndexState* st);
BOOL db_compress(Db* db, const wchar_t* out_path);

BOOL db_get_compressed_trigram(Db* db, uint32_t trigram, CompressedTrigram* out);
void db_free_compressed_trigram(CompressedTrigram* ct);

uint64_t db_intern_wstring(Db* db, const wchar_t* s);
BOOL db_put_records(Db* db, const DbRecord* recs, size_t count);
BOOL db_delete_path(Db* db, const wchar_t* parent, const wchar_t* name);
BOOL db_get_record_by_path(Db* db, const wchar_t* parent, const wchar_t* name, DbRecord* out);

#ifdef __cplusplus
}
#endif
#endif
