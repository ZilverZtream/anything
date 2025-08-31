#if defined(__linux__) || defined(__ANDROID__)
#define _XOPEN_SOURCE 700
#include "scanner.h"
#include <pthread.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#if !defined(__ANDROID__)
#include <linux/magic.h>
#endif
#include <unistd.h>
#include <ftw.h>
#include <linux/limits.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <sched.h>

#ifndef FILE_ATTRIBUTE_DIRECTORY
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#endif

struct FileScanner {
    int inotify_fd;
    pthread_t thread;
    MPMCQueue* outq;
    HANDLE cancel;
    char root[PATH_MAX];
    BOOL is_network;
    NetworkScanner* net;
};

static FileScanner* g_current;

static void emit(FileScanner* s, const char* parent, const char* name, const struct stat* st){
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
    wi->stage = INDEX_METADATA_LIGHT;
    wi->op = WI_ADD;
    while(!MPMC_Push(s->outq, wi)) sched_yield();
}

static int enum_cb(const char* fpath, const struct stat* sb, int typeflag, struct FTW* ftwbuf){
    FileScanner* s = g_current;
    if(!s) return 0;
    const char* name = fpath + ftwbuf->base;
    char parent[PATH_MAX];
    if(ftwbuf->base > 0){
        strncpy(parent, fpath, ftwbuf->base);
        parent[ftwbuf->base-1] = '\0';
    } else {
        strcpy(parent, fpath);
    }
    emit(s, parent, name, sb);
    if(typeflag == FTW_D){
        inotify_add_watch(s->inotify_fd, fpath, IN_CREATE|IN_MODIFY|IN_DELETE|IN_MOVED_TO|IN_MOVED_FROM);
    }
    return 0;
}

static void process_event(FileScanner* s, struct inotify_event* ev){
    if(ev->len == 0) return;
    if(ev->mask & (IN_DELETE|IN_MOVED_FROM)){
        DbWorkItem* wi;
        if(posix_memalign((void**)&wi, CACHE_LINE_SIZE, sizeof(DbWorkItem))!=0) return;
        wi->content = NULL;
        wi->preview = NULL;
        mbstowcs(wi->parent_path, s->root, MAX_LONG_PATH);
        mbstowcs(wi->name, ev->name, MAX_PATH);
        wi->file_size = wi->creation_time = wi->modified_time = wi->access_time = 0;
        wi->attributes = 0;
        wi->stage = INDEX_NAMES_ONLY;
        wi->op = WI_DELETE;
        while(!MPMC_Push(s->outq, wi)) sched_yield();
        return;
    }
    char full[PATH_MAX];
    snprintf(full, PATH_MAX, "%s/%s", s->root, ev->name);
    struct stat st;
    if(stat(full, &st)!=0) return;
    char parent[PATH_MAX];
    strcpy(parent, s->root);
    emit(s, parent, ev->name, &st);
}

static void* thread_proc(void* arg){
    FileScanner* s = (FileScanner*)arg;
    g_current = s;
    nftw(s->root, enum_cb, 16, FTW_PHYS);
    char buf[4096];
    for(;;){
        int len = read(s->inotify_fd, buf, sizeof(buf));
        if(len <= 0){ sched_yield(); continue; }
        for(char* p = buf; p < buf + len; ){
            struct inotify_event* ev = (struct inotify_event*)p;
            process_event(s, ev);
            p += sizeof(struct inotify_event) + ev->len;
        }
    }
    return NULL;
}

FileScanner* FileScanner_Start(const wchar_t* rootPath, int threads, MPMCQueue* outQueue, HANDLE cancelEvent){
    (void)threads; (void)cancelEvent;
    char tmp[PATH_MAX];
    wcstombs(tmp, rootPath, PATH_MAX);
#if !defined(__ANDROID__)
    struct statfs sfs;
    if(statfs(tmp, &sfs)==0){
        if(sfs.f_type==NFS_SUPER_MAGIC || sfs.f_type==SMB_SUPER_MAGIC || sfs.f_type==CIFS_SUPER_MAGIC){
            FileScanner* s = (FileScanner*)calloc(1, sizeof(FileScanner));
            if(!s) return NULL;
            s->is_network = TRUE;
            s->net = NetworkScanner_Start(rootPath, threads, outQueue, cancelEvent);
            if(!s->net){ free(s); return NULL; }
            return s;
        }
    }
#endif
    FileScanner* s = (FileScanner*)calloc(1, sizeof(FileScanner));
    if(!s) return NULL;
    wcstombs(s->root, rootPath, PATH_MAX);
    s->outq = outQueue;
    s->cancel = cancelEvent;
    s->inotify_fd = inotify_init1(0);
    if(s->inotify_fd < 0){ free(s); return NULL; }
    inotify_add_watch(s->inotify_fd, s->root, IN_CREATE|IN_MODIFY|IN_DELETE|IN_MOVED_TO|IN_MOVED_FROM);
    pthread_create(&s->thread, NULL, thread_proc, s);
    return s;
}

void FileScanner_Wait(FileScanner* s){
    if(!s) return;
    if(s->is_network) NetworkScanner_Wait(s->net);
    else pthread_join(s->thread, NULL);
}

void FileScanner_Free(FileScanner* s){
    if(!s) return;
    if(s->is_network){
        NetworkScanner_Free(s->net);
    } else {
        close(s->inotify_fd);
    }
    free(s);
}

#endif
