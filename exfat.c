
// exfat.c — Multi-threaded generic scanner with simple work stealing
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <wchar.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include "anything.h"
#include "util.h"

typedef struct DirTask {
    wchar_t path[MAX_LONG_PATH];
} DirTask;

typedef struct GenericScanner {
    HANDLE cancel;
    int threads;
    MPMCQueue* outq;
    HANDLE workers[MAX_THREADS];
    MPMCQueue local_q[MAX_THREADS];
    volatile LONG next_steal;
} GenericScanner;

static void emit(MPMCQueue* q, const wchar_t* parent, const WIN32_FIND_DATAW* f){
    if(wcscmp(f->cFileName, L".")==0 || wcscmp(f->cFileName, L"..")==0) return;
    DbWorkItem* wi = (DbWorkItem*)aligned_malloc(sizeof(DbWorkItem), CACHE_LINE_SIZE);
    wi->content = NULL;
    wi->preview = NULL;
    wcscpy_s(wi->parent_path, MAX_LONG_PATH, parent);
    wcscpy_s(wi->name, MAX_PATH, f->cFileName);
    wi->attributes = f->dwFileAttributes;
    wi->clone_id = 0;
    ULARGE_INTEGER s; s.LowPart=f->nFileSizeLow; s.HighPart=f->nFileSizeHigh; wi->file_size=s.QuadPart;
    wi->creation_time = ((ULARGE_INTEGER){.LowPart=f->ftCreationTime.dwLowDateTime,.HighPart=f->ftCreationTime.dwHighDateTime}).QuadPart;
    wi->modified_time = ((ULARGE_INTEGER){.LowPart=f->ftLastWriteTime.dwLowDateTime,.HighPart=f->ftLastWriteTime.dwHighDateTime}).QuadPart;
    wi->access_time   = ((ULARGE_INTEGER){.LowPart=f->ftLastAccessTime.dwLowDateTime,.HighPart=f->ftLastAccessTime.dwHighDateTime}).QuadPart;
    wi->stage = INDEX_METADATA_LIGHT;
    wi->op = WI_ADD;
    int tries = 1000;
    while(!MPMC_Push(q, wi) && --tries){
        SwitchToThread();
    }
    if(!tries){
        fwprintf(stderr, L"emit: dropping %ls\\%ls (queue full)\n", parent, wi->name);
        aligned_free(wi);
        return;
    }
}

static BOOL push_task(GenericScanner* s, int idx, const wchar_t* dir){
    DirTask* t = (DirTask*)malloc(sizeof(DirTask));
    if(!t) return FALSE;
    wcscpy_s(t->path, MAX_LONG_PATH, dir);
    while(!MPMC_Push(&s->local_q[idx], t)){ SwitchToThread(); }
    return TRUE;
}

static BOOL pop_task(GenericScanner* s, int idx, DirTask** out){
    if(MPMC_Pop(&s->local_q[idx], (void**)out)) return TRUE;
    // steal from others
    for(int k=0;k<s->threads;k++){
        int j = (idx + k + 1) % s->threads;
        if(MPMC_Pop(&s->local_q[j], (void**)out)) return TRUE;
    }
    return FALSE;
}

static void scan_dir(GenericScanner* s, int tidx, const wchar_t* dir){
    wchar_t pattern[MAX_LONG_PATH];
    wcscpy_s(pattern, MAX_LONG_PATH, dir);
    size_t n = wcslen(pattern);
    if(n && pattern[n-1]!=L'\\') wcscat_s(pattern, MAX_LONG_PATH, L"\\");
    wcscat_s(pattern, MAX_LONG_PATH, L"*");
    WIN32_FIND_DATAW f; HANDLE h = FindFirstFileExW(pattern, FindExInfoBasic, &f, FindExSearchNameMatch, NULL, 0);
    if(h==INVALID_HANDLE_VALUE) return;
    do{
        if(WaitForSingleObject(s->cancel, 0)==WAIT_OBJECT_0) break;
        emit(s->outq, dir, &f);
        if((f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(f.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)){
            wchar_t sub[MAX_LONG_PATH];
            path_join(sub, MAX_LONG_PATH, dir, f.cFileName);
            push_task(s, tidx, sub);
        }
    } while(FindNextFileW(h,&f));
    FindClose(h);
}

// Better: pass a little bundle
typedef struct ThreadBundle {
    GenericScanner* s;
    int idx;
} ThreadBundle;

static DWORD WINAPI worker2(void* p){
    ThreadBundle* tb = (ThreadBundle*)p;
    GenericScanner* s = tb->s; int tidx = tb->idx;
    DirTask* t=NULL;
    while(WaitForSingleObject(s->cancel, 0)!=WAIT_OBJECT_0){
        if(!pop_task(s, tidx, &t)){ Sleep(1); continue; }
        scan_dir(s, tidx, t->path);
        free(t);
    }
    free(tb);
    return 0;
}

GenericScanner* GenericScanner_Start(const wchar_t* rootPath, int threads, MPMCQueue* outQueue, HANDLE cancelEvent){
    GenericScanner* s = (GenericScanner*)calloc(1,sizeof(GenericScanner));
    s->cancel = cancelEvent; s->outq = outQueue; s->threads = threads;
    for(int i=0;i<threads;i++){
        MPMC_Init(&s->local_q[i], 1<<12);
    }
    // seed task to thread 0
    push_task(s, 0, rootPath);
    for(int i=0;i<threads;i++){
        ThreadBundle* tb = (ThreadBundle*)malloc(sizeof(ThreadBundle));
        tb->s = s; tb->idx = i;
        s->workers[i] = CreateThread(NULL,0,worker2,tb,0,NULL);
    }
    return s;
}
void GenericScanner_Wait(GenericScanner* s){
    if(!s) return;
    WaitForMultipleObjects(s->threads, s->workers, TRUE, INFINITE);
}
void GenericScanner_Free(GenericScanner* s){
    if(!s) return;
    for(int i=0;i<s->threads;i++){ CloseHandle(s->workers[i]); MPMC_Destroy(&s->local_q[i]); }
    free(s);
}
