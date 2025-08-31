// anything.c - Orchestrator for high-speed file indexing using LMDB.
// Build: cl /O2 /W3 /Gy /GL /Fe:anything.exe anything.c ntfs.c exfat.c database.c /I path\to\lmdb /link lmdb.lib shlwapi.lib

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <process.h>
#include <intrin.h>

#pragma comment(lib, "shlwapi.lib")

#include "anything.h"
#include "database.h"
#include "lmdb.h" // for MDB_MAP_FULL

// ---- MPMC Queue implementation -------------------------------------------

static inline LONG64 mpmc_min(LONG64 a, LONG64 b) { return a < b ? a : b; }

BOOL MPMC_Init(MPMCQueue* q, LONG pow2_size) {
    if (!q) return FALSE;
    LONG size = 1;
    while (size < pow2_size) size <<= 1;
    q->mask = size - 1;

    size_t cells_size = sizeof(MPMCCell) * (size_t)size;
    q->cells = (MPMCCell*)_aligned_malloc(cells_size, CACHE_LINE_SIZE);
    if (!q->cells) return FALSE;
    ZeroMemory(q->cells, cells_size);
    for (LONG i=0; i<size; ++i) {
        q->cells[i].seq = i;
        q->cells[i].data = NULL;
    }
    q->head = 0;
    q->tail = 0;
    return TRUE;
}
void MPMC_Destroy(MPMCQueue* q) {
    if (!q) return;
    if (q->cells) _aligned_free(q->cells);
    ZeroMemory(q, sizeof(*q));
}
BOOL MPMC_Push(MPMCQueue* q, void* val) {
    MPMCCell* cells = q->cells;
    LONG64 mask = q->mask;
    for (;;) {
        LONG64 pos = q->tail;
        MPMCCell* cell = &cells[pos & mask];
        LONG64 seq = cell->seq;
        LONG64 dif = seq - pos;
        if (dif == 0) {
            if (_InterlockedCompareExchange64(&q->tail, pos+1, pos) == pos) {
                cell->data = val;
                cell->seq = pos + 1;
                return TRUE;
            }
        } else if (dif < 0) {
            return FALSE; // full
        } else {
            YieldProcessor();
        }
    }
}
BOOL MPMC_Pop(MPMCQueue* q, void** out) {
    MPMCCell* cells = q->cells;
    LONG64 mask = q->mask;
    for (;;) {
        LONG64 pos = q->head;
        MPMCCell* cell = &cells[pos & mask];
        LONG64 seq = cell->seq;
        LONG64 dif = seq - (pos + 1);
        if (dif == 0) {
            if (_InterlockedCompareExchange64(&q->head, pos+1, pos) == pos) {
                void* data = cell->data;
                cell->seq = pos + mask + 1;
                *out = data;
                return TRUE;
            }
        } else if (dif < 0) {
            return FALSE;
        } else {
            YieldProcessor();
        }
    }
}

// ---- Args -----------------------------------------------------------------

typedef struct {
    WCHAR startPath[MAX_LONG_PATH];
    WCHAR dbPath[MAX_PATH];
    int   threads;
    int   batch;
    size_t map_init_mb;
    size_t map_max_mb;
    BOOL  force_generic;
    BOOL  force_ntfs;
} AppArgs;

static void print_usage() {
    puts("ANYTHING - ultra-fast file indexer (Windows)\n");
    puts("Usage:");
    puts("  anything.exe <start-path> <db-path> [options]\n");
    puts("Options:");
    puts("  -threads N     Number of worker threads (default: all CPUs)");
    puts("  -batch N       LMDB batch size (default: 50000)");
    puts("  -mapinit N     Initial LMDB map size in MB (default: 512)");
    puts("  -mapmax N      Maximum LMDB map size in MB (default: 16384)");
    puts("  -generic       Force generic file scanner");
    puts("  -ntfs          Force NTFS USN scanner");
    puts("  -help          Show this help\n");
}

static BOOL parse_args(int argc, char** argv, AppArgs* args) {
    if (argc < 3) {
        print_usage();
        return FALSE;
    }
    ZeroMemory(args, sizeof(*args));
    GetFullPathNameA(argv[1], MAX_LONG_PATH, args->startPath, NULL);
    GetFullPathNameA(argv[2], MAX_PATH, args->dbPath, NULL);
    args->threads = 0;
    args->batch = LMDB_BATCH_SIZE_SUGGESTION;
    args->map_init_mb = 512;
    args->map_max_mb = 16384;

    for (int i=3; i<argc; ++i) {
        if (_stricmp(argv[i], "-threads") == 0 && i+1 < argc) {
            args->threads = atoi(argv[++i]);
        } else if (_stricmp(argv[i], "-batch") == 0 && i+1 < argc) {
            args->batch = atoi(argv[++i]);
        } else if (_stricmp(argv[i], "-mapinit") == 0 && i+1 < argc) {
            args->map_init_mb = (size_t)atoi(argv[++i]);
        } else if (_stricmp(argv[i], "-mapmax") == 0 && i+1 < argc) {
            args->map_max_mb = (size_t)atoi(argv[++i]);
        } else if (_stricmp(argv[i], "-generic") == 0) {
            args->force_generic = TRUE;
        } else if (_stricmp(argv[i], "-ntfs") == 0) {
            args->force_ntfs = TRUE;
        } else if (_stricmp(argv[i], "-help") == 0) {
            print_usage();
            exit(0);
        } else {
            printf("Unknown option: %s\n", argv[i]);
            print_usage();
            return FALSE;
        }
    }
    return TRUE;
}

// ---- Worker orchestrator --------------------------------------------------

static DWORD WINAPI DbWriterThread(void* p) {
    WriterCtx* ctx = (WriterCtx*)p;
    void* item;
    RecordBatch* batch = (RecordBatch*)malloc(sizeof(RecordBatch));
    batch->count = 0;

    for (;;) {
        if (!MPMC_Pop(&ctx->queue, &item)) {
            if (ctx->done) break;
            Sleep(1);
            continue;
        }
        if (item == NULL) {
            break;
        }
        Record* rec = (Record*)item;
        batch->items[batch->count++] = *rec;
        free(rec);
        if (batch->count >= ctx->batch_size) {
            db_write_batch(ctx->db, batch->items, batch->count);
            batch->count = 0;
        }
    }
    if (batch->count > 0) {
        db_write_batch(ctx->db, batch->items, batch->count);
    }
    free(batch);
    return 0;
}

int main(int argc, char** argv) {
    AppArgs args;
    if (!parse_args(argc, argv, &args)) {
        return 1;
    }
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    if (args.threads <= 0) args.threads = sysinfo.dwNumberOfProcessors;

    printf("Starting scan: %S\n", args.startPath);
    printf("Database: %S\n", args.dbPath);
    printf("Threads: %d, Batch: %d, MapInit: %zu MB, MapMax: %zu MB\n",
        args.threads, args.batch, args.map_init_mb, args.map_max_mb);

    DbHandle* db = db_create(args.dbPath, args.map_init_mb*1024*1024, args.map_max_mb*1024*1024);
    if (!db) {
        fprintf(stderr, "Failed to create database.\n");
        return 1;
    }

    WriterCtx ctx;
    ctx.db = db;
    ctx.batch_size = args.batch;
    ctx.done = FALSE;
    if (!MPMC_Init(&ctx.queue, 1<<16)) {
        fprintf(stderr, "Queue init failed\n");
        return 1;
    }
    HANDLE writer = CreateThread(NULL, 0, DbWriterThread, &ctx, 0, NULL);

    BOOL use_ntfs = args.force_ntfs;
    BOOL use_generic = args.force_generic;
    if (!use_ntfs && !use_generic) {
        WCHAR fsname[MAX_PATH];
        if (GetVolumeInformationW(args.startPath, NULL, 0, NULL, NULL, NULL, fsname, MAX_PATH)) {
            if (_wcsicmp(fsname, L"NTFS") == 0) {
                use_ntfs = TRUE;
            } else {
                use_generic = TRUE;
            }
        } else {
            use_generic = TRUE;
        }
    }

    if (use_ntfs) {
        IndexVolume_NTFS(args.startPath, &ctx, args.threads);
    } else {
        IndexVolume_Generic(args.startPath, &ctx, args.threads);
    }

    ctx.done = TRUE;
    WaitForSingleObject(writer, INFINITE);
    CloseHandle(writer);

    db_close(db);
    const DbHeader* header = db_open_readonly(args.dbPath, &db);
    if (header) {
        printf("Total records: %llu\n", (unsigned long long)header->record_count);
        db_close(db);
    }

    MPMC_Destroy(&ctx.queue);
    return 0;
}
