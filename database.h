
#ifndef DATABASE_H
#define DATABASE_H
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {


#endif

typedef struct Db Db;
typedef struct DbRecord DbRecord;
typedef struct DbHeader DbHeader;
typedef struct IndexState {
    uint64_t dummy; // placeholder for future incremental index metadata
} IndexState;

BOOL db_create(const wchar_t* path, size_t map_init_mb, size_t map_max_mb, Db** out_db);
const DbHeader* db_open_readonly(const wchar_t* path, Db** out_db);
void db_close(Db* db);

const DbHeader* db_header(Db* db);
size_t db_current_mapsize(Db* db);
size_t db_max_mapsize(Db* db);
BOOL   db_set_mapsize(Db* db, size_t new_size_bytes);
int    db_last_error(Db* db);

BOOL db_begin_write(Db* db);
BOOL db_commit_write(Db* db);
void db_abort_write(Db* db);

BOOL db_get_index_state(Db* db, IndexState* out);
BOOL db_set_index_state(Db* db, const IndexState* st);

uint64_t db_intern_wstring(Db* db, const wchar_t* s);
BOOL db_put_records(Db* db, const DbRecord* recs, size_t count);

#ifdef __cplusplus
}
#endif
#endif


