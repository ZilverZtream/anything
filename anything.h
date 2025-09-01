
#ifndef ANYTHING_H
#define ANYTHING_H
#define _CRT_SECURE_NO_WARNINGS
#include <stdint.h>
#include <stdbool.h>
#include <wchar.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sys/types.h>
// Use pthread_t for HANDLE on non-Windows platforms to provide a
// concrete native handle type rather than a generic void pointer.
typedef pthread_t HANDLE;
typedef int BOOL;
#include <stdint.h>
typedef int32_t LONG;
typedef int64_t LONG64;
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#ifndef MAX_PATH
#define MAX_PATH 260
#endif
#endif

#ifndef CACHE_LINE_SIZE
#define CACHE_LINE_SIZE 64
#endif
#ifndef MAX_LONG_PATH
#define MAX_LONG_PATH 32768
#endif
#ifndef MAX_THREADS
#define MAX_THREADS 64
#endif

#define FILE_ATTRIBUTE_SNAPSHOT 0x20000000u
#define FILE_ATTRIBUTE_CLONE    0x40000000u

typedef enum {
    DB_REC_FILE = 1,
    DB_REC_DIR  = 2
} DbRecordType;

typedef enum {
    INDEX_NAMES_ONLY = 1,     // Everything-like speed
    INDEX_METADATA_LIGHT,     // Basic file properties
    INDEX_FULL_CONTENT        // Complete text extraction
} IndexingLevel;

typedef struct Db Db;
typedef struct IndexState IndexState;

typedef struct DbRecord {
    uint64_t  rec_id;
    uint64_t  parent_str_id;
    uint64_t  name_str_id;
    uint64_t  file_size;
    uint64_t  creation_time;
    uint64_t  modified_time;
    uint64_t  access_time;
    uint32_t  attributes;
    uint8_t   type;
    uint64_t  content_str_id;
    uint64_t  author_str_id;
    uint64_t  camera_str_id;
    uint64_t  lens_str_id;
    uint64_t  artist_str_id;
    uint64_t  album_str_id;
    uint64_t  title_str_id;
    uint64_t  preview_str_id;
    uint64_t  hash_crc;
} DbRecord;

typedef struct DbHeader {
    uint64_t record_count;
    uint64_t string_count;
    uint64_t created_time;
    uint64_t updated_time;
    uint64_t map_size_bytes;
} DbHeader;

typedef struct DbWorkItem {
    wchar_t  parent_path[MAX_LONG_PATH];
    wchar_t  name[MAX_PATH];
    uint64_t file_size;
    uint64_t creation_time;
    uint64_t modified_time;
    uint64_t access_time;
    uint32_t attributes;
    IndexingLevel stage; // INDEX_NAMES_ONLY, INDEX_METADATA_LIGHT, INDEX_FULL_CONTENT
    uint8_t  op; // 0 = add/update, 1 = delete
    wchar_t* content; // optional extracted text
    wchar_t* preview; // optional preview text or thumbnail path
    uint64_t clone_id; // APFS clone identifier (0 if not clone)
} DbWorkItem;

#define WI_ADD    0
#define WI_DELETE 1

typedef struct LiveUpdate {
    wchar_t  parent_path[MAX_LONG_PATH];
    wchar_t  name[MAX_PATH];
    uint8_t  op;
} LiveUpdate;

void live_updates_init(void);
BOOL live_updates_poll(LiveUpdate* out);

// ---- MPMC lock-free queue (Vyukov) ----
typedef struct MPMCCell {
    volatile LONG64 seq;
    void*           data;
    char            pad[CACHE_LINE_SIZE - sizeof(LONG64) - sizeof(void*)];
} MPMCCell;

typedef struct MPMCQueue {
    LONG64      mask;
    MPMCCell*   cells;
    volatile LONG64 head;
    char        pad1[CACHE_LINE_SIZE - sizeof(LONG64)];
    volatile LONG64 tail;
    char        pad2[CACHE_LINE_SIZE - sizeof(LONG64)];
} MPMCQueue;

BOOL MPMC_Init(MPMCQueue* q, LONG pow2_size);
void MPMC_Destroy(MPMCQueue* q);
BOOL MPMC_Push(MPMCQueue* q, void* data);
BOOL MPMC_Pop(MPMCQueue* q, void** out);

// ---- Generic scanner (multi-threaded with work stealing) ----
typedef struct GenericScanner GenericScanner;
GenericScanner* GenericScanner_Start(const wchar_t* rootPath, int threads, MPMCQueue* outQueue, HANDLE cancelEvent);
void GenericScanner_Wait(GenericScanner* s);
void GenericScanner_Free(GenericScanner* s);

// ---- Network share scanner ----
typedef struct NetworkScanner NetworkScanner;
NetworkScanner* NetworkScanner_Start(const wchar_t* rootPath, int threads, MPMCQueue* outQueue, HANDLE cancelEvent);
void NetworkScanner_Wait(NetworkScanner* s);
void NetworkScanner_Free(NetworkScanner* s);

// ---- NTFS USN-based scanner ----
typedef struct NTFSScanner NTFSScanner;
NTFSScanner* NTFSScanner_Start(const wchar_t* volumeRoot, int threads, MPMCQueue* outQueue, HANDLE cancelEvent);
void NTFSScanner_Wait(NTFSScanner* s);
void NTFSScanner_Free(NTFSScanner* s);

// Change tailer via USN journal
#ifdef _WIN32
HANDLE StartUSNTailer(const wchar_t* volumeRoot, MPMCQueue* outQueue, HANDLE cancelEvent);
#endif

// ---- DB API (LMDB) ----
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
BOOL db_compress(Db* db, const wchar_t* out_path);

uint64_t db_intern_wstring(Db* db, const wchar_t* s);
BOOL db_put_records(Db* db, const DbRecord* recs, size_t count);

#endif
