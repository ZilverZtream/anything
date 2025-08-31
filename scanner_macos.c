#ifdef __APPLE__
#include "scanner.h"
#include <CoreServices/CoreServices.h>
#include <pthread.h>
#include <sys/stat.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <ftw.h>

#ifndef FILE_ATTRIBUTE_DIRECTORY
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#endif

struct FileScanner {
    FSEventStreamRef stream;
    pthread_t thread;
    MPMCQueue* outq;
    char root[PATH_MAX];
};

static FileScanner* g_current;

static void emit(FileScanner* s, const char* path){
    struct stat st;
    if(stat(path,&st)!=0) return;
    char parent[PATH_MAX];
    const char* name = strrchr(path,'/');
    if(name){
        size_t len = name - path;
        strncpy(parent, path, len);
        parent[len]=0;
        name++;
    } else {
        parent[0]=0;
        name = path;
    }
    DbWorkItem* wi;
    if(posix_memalign((void**)&wi, CACHE_LINE_SIZE, sizeof(DbWorkItem))!=0) return;
    mbstowcs(wi->parent_path, parent, MAX_LONG_PATH);
    mbstowcs(wi->name, name, MAX_PATH);
    wi->file_size = st.st_size;
    wi->creation_time = st.st_mtime;
    wi->modified_time = st.st_mtime;
    wi->access_time = st.st_atime;
    wi->attributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : 0;
    while(!MPMC_Push(s->outq, wi)) sched_yield();
}

static int enum_cb(const char* fpath, const struct stat* sb, int typeflag, struct FTW* ftwbuf){
    (void)sb; (void)typeflag; (void)ftwbuf;
    emit(g_current, fpath);
    return 0;
}

static void fsevent_cb(ConstFSEventStreamRef streamRef,
                       void *clientCallBackInfo,
                       size_t numEvents,
                       void *eventPaths,
                       const FSEventStreamEventFlags eventFlags[],
                       const FSEventStreamEventId eventIds[]){
    (void)streamRef; (void)eventFlags; (void)eventIds;
    FileScanner* s = (FileScanner*)clientCallBackInfo;
    char** paths = (char**)eventPaths;
    for(size_t i=0;i<numEvents;i++){
        emit(s, paths[i]);
    }
}

static void* runloop_thread(void* arg){
    FileScanner* s = (FileScanner*)arg;
    g_current = s;
    nftw(s->root, enum_cb, 16, FTW_PHYS);
    FSEventStreamScheduleWithRunLoop(s->stream, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    FSEventStreamStart(s->stream);
    CFRunLoopRun();
    return NULL;
}

FileScanner* FileScanner_Start(const wchar_t* rootPath, int threads, MPMCQueue* outQueue, HANDLE cancelEvent){
    (void)threads; (void)cancelEvent;
    FileScanner* s = (FileScanner*)calloc(1,sizeof(FileScanner));
    if(!s) return NULL;
    s->outq = outQueue;
    wcstombs(s->root, rootPath, PATH_MAX);
    CFStringRef path = CFStringCreateWithCString(NULL, s->root, kCFStringEncodingUTF8);
    CFArrayRef paths = CFArrayCreate(NULL, (const void**)&path, 1, &kCFTypeArrayCallBacks);
    FSEventStreamContext ctx = {0, s, NULL, NULL, NULL};
    s->stream = FSEventStreamCreate(NULL, fsevent_cb, &ctx, paths,
                                    kFSEventStreamEventIdSinceNow, 0.5,
                                    kFSEventStreamCreateFlagFileEvents);
    CFRelease(path);
    CFRelease(paths);
    if(!s->stream){ free(s); return NULL; }
    pthread_create(&s->thread, NULL, runloop_thread, s);
    return s;
}

void FileScanner_Wait(FileScanner* s){
    if(!s) return;
    pthread_join(s->thread, NULL);
}

void FileScanner_Free(FileScanner* s){
    if(!s) return;
    FSEventStreamStop(s->stream);
    FSEventStreamInvalidate(s->stream);
    FSEventStreamRelease(s->stream);
    free(s);
}

#endif
