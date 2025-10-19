#pragma once

#ifndef DATABASE_H
#define DATABASE_H
#ifdef _WIN32
#include <windows.h>
#else
#include <stddef.h>
typedef int BOOL;
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#endif
#include <stdint.h>
#include <stdbool.h>
#include <wchar.h>
#include "anything/anything.h"

#define DB_BLOOM_MAX_BYTES      (128*1024)  // only index first 128KB of any string
#define DB_BLOOM_STRIDE_AFTER   (64*1024)   // process every trigram up to 64KB
#define DB_BLOOM_STRIDE         2           // then sample every other trigram

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

#define STRING_META_MAGIC0 'B'
#define STRING_META_MAGIC1 'F'

typedef struct {
    uint32_t trigram_count;
    uint8_t  hash_count;
    uint8_t  bloom_log2;
    uint8_t  magic0;
    uint8_t  magic1;
    uint64_t bloom_offset;
    uint32_t bloom_length;
    uint8_t  bloom_pending;
    uint8_t  _reserved[3];
} StringMeta;

typedef struct {
    uint64_t   string_id;
    StringMeta new_meta;
    uint8_t*   bloom_data;
    size_t     bloom_data_len;
} BloomResultItem;

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
BOOL   db_set_bulk_mode(Db* db, BOOL enable, uint32_t sync_interval_seconds);
const DbError* db_last_error(Db* db);
size_t db_last_write_progress(Db* db);

BOOL db_ensure_loaded(Db* db, IndexLoadState state);

BOOL db_begin_write(Db* db);
BOOL db_commit_write_ex(Db* db, BOOL force_sync);
BOOL db_commit_write(Db* db);
void db_abort_write(Db* db);

BOOL db_get_index_state(Db* db, IndexState* out);
BOOL db_set_index_state(Db* db, const IndexState* st);
BOOL db_compress(Db* db, const wchar_t* out_path);

// Internal helpers exposed for lazy bloom generation.
BOOL db_string_value_parse(const MDB_val* value, MDB_val* text, StringMeta* meta_out, BOOL* has_meta);
BOOL db_generate_bloom_blob(const char* text_utf8, size_t text_len, StringMeta* meta_out, uint8_t** bloom_data_out, size_t* bloom_len_out);
BOOL db_apply_generated_bloom(Db* db, const BloomResultItem* result);

extern MPMCQueue g_bloom_gen_queue;
extern MPMCQueue g_bloom_completion_queue;

void bloom_generator_init(const wchar_t* dbPath);
void bloom_generator_shutdown(void);
void bloom_generator_request(uint64_t string_id);

BOOL db_get_compressed_trigram(Db* db, uint32_t trigram, CompressedTrigram* out);
void db_free_compressed_trigram(CompressedTrigram* ct);

uint64_t db_intern_wstring(Db* db, const wchar_t* s);
void db_intern_wstrings_batched(Db* db,
                                const wchar_t* const* strings,
                                const uint8_t* need_normalized_flags,
                                size_t count,
                                uint64_t* out_ids,
                                uint64_t* out_normalized_ids);
BOOL db_put_records(Db* db, const DbRecord* recs, size_t count);
BOOL db_delete_path(Db* db, const wchar_t* parent, const wchar_t* name);
BOOL db_get_record_by_path(Db* db, const wchar_t* parent, const wchar_t* name, DbRecord* out);

BOOL db_parent_cache_copy(uint64_t parent_str_id, char* buffer, size_t buffer_len, size_t* out_len);
void db_parent_cache_put(uint64_t parent_str_id, const char* path_utf8, size_t path_len);
void db_parent_cache_reset(void);

#ifdef __cplusplus
}
#endif
#endif
