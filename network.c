#include "scanner.h"
#include <stdlib.h>

#ifdef _WIN32

// On Windows network shares behave like generic file systems. Wrap the
// existing GenericScanner to provide a dedicated NetworkScanner interface.

struct NetworkScanner {
    GenericScanner* gen;
};

NetworkScanner* NetworkScanner_Start(const wchar_t* rootPath, int threads, MPMCQueue* outQueue, HANDLE cancelEvent){
    NetworkScanner* s = (NetworkScanner*)calloc(1, sizeof(NetworkScanner));
    if(!s) return NULL;
    s->gen = GenericScanner_Start(rootPath, threads, outQueue, cancelEvent);
    if(!s->gen){ free(s); return NULL; }
    return s;
}

void NetworkScanner_Wait(NetworkScanner* s){
    if(!s) return;
    GenericScanner_Wait(s->gen);
}

void NetworkScanner_Free(NetworkScanner* s){
    if(!s) return;
    GenericScanner_Free(s->gen);
    free(s);
}

#endif // _WIN32

#ifdef __linux__

#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ftw.h>
#include <linux/limits.h>
#include <string.h>
#include <sched.h>
#include <errno.h>

#ifndef FILE_ATTRIBUTE_DIRECTORY
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#endif

struct NetworkScanner {
    pthread_t thread;
    MPMCQueue* outq;
    HANDLE cancel;
    char root[PATH_MAX];
};

static struct NetworkScanner* g_net_current;

static void emit_net(struct NetworkScanner* s, const char* parent, const char* name, const struct stat* st){
    DbWorkItem* wi;
    if(posix_memalign((void**)&wi, CACHE_LINE_SIZE, sizeof(DbWorkItem))!=0) return;
    wi->content = NULL;
    wi->preview = NULL;
    mbstowcs(wi->parent_path, parent, MAX_LONG_PATH);
    mbstowcs(wi->name, name, MAX_PATH);
    wi->file_size = st->st_size;
    wi->creation_time = st->st_mtime;
    wi->modified_time = st->st_mtime;
    wi->access_time = st->st_atime;
    wi->attributes = S_ISDIR(st->st_mode) ? FILE_ATTRIBUTE_DIRECTORY : 0;
    wi->op = WI_ADD;
    while(!MPMC_Push(s->outq, wi)) sched_yield();
}

static int enum_cb_net(const char* fpath, const struct stat* sb, int typeflag, struct FTW* ftwbuf){
    (void)typeflag;
    struct NetworkScanner* s = g_net_current;
    if(!s) return 0;
    const char* name = fpath + ftwbuf->base;
    char parent[PATH_MAX];
    if(ftwbuf->base > 0){
        strncpy(parent, fpath, ftwbuf->base);
        parent[ftwbuf->base-1] = '\0';
    } else {
        strcpy(parent, fpath);
    }
    emit_net(s, parent, name, sb);
    return 0;
}

static void* net_thread(void* arg){
    struct NetworkScanner* s = (struct NetworkScanner*)arg;
    g_net_current = s;
    nftw(s->root, enum_cb_net, 16, FTW_PHYS);
    return NULL;
}

NetworkScanner* NetworkScanner_Start(const wchar_t* rootPath, int threads, MPMCQueue* outQueue, HANDLE cancelEvent){
    (void)threads; (void)cancelEvent;
    struct NetworkScanner* s = (struct NetworkScanner*)calloc(1, sizeof(struct NetworkScanner));
    if(!s) return NULL;
    wcstombs(s->root, rootPath, PATH_MAX);
    s->outq = outQueue;
    s->cancel = cancelEvent;
    pthread_create(&s->thread, NULL, net_thread, s);
    return s;
}

void NetworkScanner_Wait(struct NetworkScanner* s){
    if(!s) return;
    pthread_join(s->thread, NULL);
}

void NetworkScanner_Free(struct NetworkScanner* s){
    if(!s) return;
    free(s);
}

#endif // __linux__

