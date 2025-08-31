#ifndef ANYTHING_H
#define ANYTHING_H

#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include "database.h"

// Long-path support up to Win32 extended-length prefix \\?\
#ifndef MAX_LONG_PATH
#define MAX_LONG_PATH 32768
#endif

#ifndef MAX_THREADS
#define MAX_THREADS 64
#endif

#ifndef CACHE_LINE_SIZE
#define CACHE_LINE_SIZE 64
#endif

// ---- Work item passed from scanners to DB writer --------------------------
typedef struct {
    DbRecordType type;
    WCHAR        parent_path[MAX_LONG_PATH]; // full parent directory path (extended-length)
    WCHAR        name[MAX_PATH];             // file/dir name only
    uint64_t     file_size;
    uint64_t     creation_time;
    uint64_t     modified_time;
    uint64_t     access_time;
    uint32_t     attributes;
} DbWorkItem;

// ---- MPMC Queue (bounded, lock-free, 64-bit counters) ---------------------

typedef struct {
    volatile LONG64 seq;
    void*           data;
    char            pad[CACHE_LINE_SIZE - sizeof(LONG64) - sizeof(void*)];
} MPMCCell;

typedef struct MPMCQueue {
    LONG64     mask;
    MPMCCell*  cells;
    LONG64     head;
    char       pad1[CACHE_LINE_SIZE - sizeof(LONG64)];
    LONG64     tail;
    char       pad2[CACHE_LINE_SIZE - sizeof(LONG64)];
} MPMCQueue;

BOOL MPMC_Init(MPMCQueue* q, LONG pow2_size);
void MPMC_Destroy(MPMCQueue* q);
BOOL MPMC_Push(MPMCQueue* q, void* val);
BOOL MPMC_Pop(MPMCQueue* q, void** out);

// ---- Scanner start functions ----------------------------------------------

// Generic (portable) scanner using FindFirstFileExW, safe for NTFS/ExFAT/FAT/UNC.
typedef struct GenericScanner GenericScanner;
GenericScanner* GenericScanner_Start(const WCHAR* rootPath, int threads, MPMCQueue* outQueue, HANDLE cancelEvent);
void GenericScanner_Wait(GenericScanner* s);
void GenericScanner_Free(GenericScanner* s);

// NTFS USN-based scanner (fast path). Only valid for local NTFS volumes (X:\).
typedef struct NTFSScanner NTFSScanner;
NTFSScanner* NTFSScanner_Start(const WCHAR* volumeRoot, int threads, MPMCQueue* outQueue, HANDLE cancelEvent);
void NTFSScanner_Wait(NTFSScanner* s);
void NTFSScanner_Free(NTFSScanner* s);

#endif // ANYTHING_H
