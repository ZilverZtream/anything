// exfat.c - Generic filesystem scanner (FindFirstFileExW-based) for ANYTHING
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <process.h>

#pragma comment(lib, "shlwapi.lib")

#include "anything.h"

typedef struct DirNode {
    struct DirNode* next;
    WCHAR path[MAX_LONG_PATH];
} DirNode;

struct GenericScanner {
    HANDLE* threads;
    int     thread_count;
    HANDLE  cancel_event;
    // simple mutex-protected LIFO stack for directory tasks
    SRWLOCK dir_lock;
    DirNode* dir_head;
    MPMCQueue* out_q;
};

// Helpers -------------------------------------------------------------------

static inline BOOL is_dot_dir(const WCHAR* name) {
    return (name[0]==L'.' && (name[1]==0 || (name[1]==L'.' && name[2]==0)));
}

static inline void u64_from_filetime(const FILETIME* ft, uint64_t* out) {
    ULARGE_INTEGER u; u.LowPart = ft->dwLowDateTime; u.HighPart = ft->dwHighDateTime;
    *out = u.QuadPart;
}

static void push_dir(GenericScanner* s, const WCHAR* path) {
    DirNode* n = (DirNode*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DirNode));
    if (!n) return;
    wcsncpy_s(n->path, MAX_LONG_PATH, path, _TRUNCATE);
    AcquireSRWLockExclusive(&s->dir_lock);
    n->next = s->dir_head;
    s->dir_head = n;
    ReleaseSRWLockExclusive(&s->dir_lock);
}

static BOOL pop_dir(GenericScanner* s, WCHAR* out, size_t outcch) {
    AcquireSRWLockExclusive(&s->dir_lock);
    DirNode* n = s->dir_head;
    if (!n) { ReleaseSRWLockExclusive(&s->dir_lock); return FALSE; }
    s->dir_head = n->next;
    ReleaseSRWLockExclusive(&s->dir_lock);
    wcsncpy_s(out, outcch, n->path, _TRUNCATE);
    HeapFree(GetProcessHeap(), 0, n);
    return TRUE;
}

static void emit_item(GenericScanner* s, const WCHAR* parent, const WIN32_FIND_DATAW* f) {
    DbWorkItem* wi = (DbWorkItem*)_aligned_malloc(sizeof(DbWorkItem), CACHE_LINE_SIZE);
    if (!wi) return;
    ZeroMemory(wi, sizeof(*wi));

    BOOL is_dir = (f->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

    wi->type = is_dir ? DB_REC_DIR : DB_REC_FILE;
    wcsncpy_s(wi->parent_path, MAX_LONG_PATH, parent, _TRUNCATE);
    wcsncpy_s(wi->name, MAX_PATH, f->cFileName, _TRUNCATE);
    wi->attributes = f->dwFileAttributes;

    ULARGE_INTEGER sz;
    sz.LowPart  = f->nFileSizeLow;
    sz.HighPart = f->nFileSizeHigh;
    wi->file_size = sz.QuadPart;

    u64_from_filetime(&f->ftCreationTime, &wi->creation_time);
    u64_from_filetime(&f->ftLastWriteTime, &wi->modified_time);
    u64_from_filetime(&f->ftLastAccessTime, &wi->access_time);

    // Push; if full, spin/yield
    while (!MPMC_Push(s->out_q, wi)) {
        if (WaitForSingleObject(s->cancel_event, 0) == WAIT_OBJECT_0) {
            _aligned_free(wi);
            return;
        }
        SwitchToThread();
    }
}

static unsigned __stdcall generic_worker(void* param) {
    GenericScanner* s = (GenericScanner*)param;
    WCHAR dir[MAX_LONG_PATH];

    while (WaitForSingleObject(s->cancel_event, 0) == WAIT_TIMEOUT) {
        if (!pop_dir(s, dir, MAX_LONG_PATH)) {
            // No more work right now
            Sleep(10);
            continue;
        }

        // Build search pattern
        WCHAR pattern[MAX_LONG_PATH];
        wcsncpy_s(pattern, MAX_LONG_PATH, dir, _TRUNCATE);
        size_t len = wcslen(pattern);
        if (len && pattern[len-1] != L'\\') {
            wcscat_s(pattern, MAX_LONG_PATH, L"\\");
        }
        wcscat_s(pattern, MAX_LONG_PATH, L"*");

        WIN32_FIND_DATAW f; ZeroMemory(&f, sizeof(f));
        HANDLE h = FindFirstFileExW(pattern, FindExInfoBasic, &f, FindExSearchNameMatch, NULL,
                                    FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) {
            continue;
        }

        do {
            if (is_dot_dir(f.cFileName)) continue;

            emit_item(s, dir, &f);

            if ((f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                // Do not traverse into reparse points to avoid cycles
                if ((f.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
                    WCHAR next_dir[MAX_LONG_PATH];
                    wcsncpy_s(next_dir, MAX_LONG_PATH, dir, _TRUNCATE);
                    size_t l2 = wcslen(next_dir);
                    if (l2 && next_dir[l2-1] != L'\\') wcscat_s(next_dir, MAX_LONG_PATH, L"\\");
                    wcscat_s(next_dir, MAX_LONG_PATH, f.cFileName);
                    push_dir(s, next_dir);
                }
            }
        } while (FindNextFileW(h, &f));

        FindClose(h);
    }

    return 0;
}

static void ensure_extended_path(const WCHAR* in, WCHAR* out, size_t outcch) {
    if (wcsncmp(in, L"\\\\?\\", 4) == 0) {
        wcsncpy_s(out, outcch, in, _TRUNCATE);
        return;
    }
    // handle UNC vs local drive
    if (wcsncmp(in, L"\\\\", 2) == 0) {
        // UNC: \\server\share -> \\?\UNC\server\share
        swprintf_s(out, outcch, L"\\\\?\\UNC\\%s", in+2);
    } else {
        swprintf_s(out, outcch, L"\\\\?\\%s", in);
    }
}

GenericScanner* GenericScanner_Start(const WCHAR* rootPath, int threads, MPMCQueue* outQueue, HANDLE cancelEvent) {
    GenericScanner* s = (GenericScanner*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(GenericScanner));
    if (!s) return NULL;
    InitializeSRWLock(&s->dir_lock);
    s->out_q = outQueue;
    s->cancel_event = cancelEvent;

    WCHAR root_ext[MAX_LONG_PATH];
    ensure_extended_path(rootPath, root_ext, MAX_LONG_PATH);
    push_dir(s, root_ext);

    s->thread_count = (threads > 0 && threads <= MAX_THREADS) ? threads : 1;
    s->threads = (HANDLE*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(HANDLE)*s->thread_count);
    if (!s->threads) { HeapFree(GetProcessHeap(),0,s); return NULL; }

    for (int i=0;i<s->thread_count;i++) {
        uintptr_t th = _beginthreadex(NULL, 0, generic_worker, s, 0, NULL);
        s->threads[i] = (HANDLE)th;
    }

    return s;
}

void GenericScanner_Wait(GenericScanner* s) {
    if (!s) return;
    WaitForMultipleObjects(s->thread_count, s->threads, TRUE, INFINITE);
}

void GenericScanner_Free(GenericScanner* s) {
    if (!s) return;
    for (int i=0;i<s->thread_count;i++) {
        if (s->threads[i]) CloseHandle(s->threads[i]);
    }
    HeapFree(GetProcessHeap(), 0, s->threads);
    // free any leftover dir nodes
    AcquireSRWLockExclusive(&s->dir_lock);
    DirNode* n = s->dir_head;
    s->dir_head = NULL;
    ReleaseSRWLockExclusive(&s->dir_lock);
    while (n) {
        DirNode* nx = n->next;
        HeapFree(GetProcessHeap(), 0, n);
        n = nx;
    }
    HeapFree(GetProcessHeap(), 0, s);
}
