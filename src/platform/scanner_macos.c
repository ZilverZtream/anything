#ifdef __APPLE__
#include "core/pch.h"
#include "apfs.h"
#include <CoreServices/CoreServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <QuickLook/QuickLook.h>
#include <ImageIO/ImageIO.h>
#include <uuid/uuid.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <sched.h>
#include <ftw.h>
#include <zip.h>

#ifndef FILE_ATTRIBUTE_DIRECTORY
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#endif

struct FileScanner {
    FSEventStreamRef stream;
    pthread_t loop_thread;
    pthread_t enum_thread;
    CFRunLoopRef run_loop;
    volatile BOOL stop;
    CancelToken* cancel;
    MPMCQueue* outq;
    char root[PATH_MAX];
    BOOL is_network;
    NetworkScanner* net;
};

static FileScanner* g_current;

wchar_t* GenerateThumbnail(const wchar_t* path){
    CFStringRef cfPath = CFStringCreateWithCharacters(NULL, path, wcslen(path));
    if(!cfPath) return NULL;
    CFURLRef url = CFURLCreateWithFileSystemPath(NULL, cfPath, kCFURLPOSIXPathStyle, false);
    CFRelease(cfPath);
    if(!url) return NULL;
    CGSize size = {256,256};
    CGImageRef img = QLThumbnailImageCreate(NULL, url, size, NULL);
    CFRelease(url);
    if(!img) return NULL;
    uuid_t id; uuid_generate(id);
    char idstr[37]; uuid_unparse_lower(id, idstr);
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "/tmp/%s.png", idstr);
    CFURLRef dst = CFURLCreateFromFileSystemRepresentation(NULL, (UInt8*)tmp, strlen(tmp), false);
    if(!dst){ CGImageRelease(img); return NULL; }
    CGImageDestinationRef dest = CGImageDestinationCreateWithURL(dst, kUTTypePNG, 1, NULL);
    if(!dest){ CFRelease(dst); CGImageRelease(img); return NULL; }
    CGImageDestinationAddImage(dest, img, NULL);
    bool ok = CGImageDestinationFinalize(dest);
    CFRelease(dest);
    CFRelease(dst);
    CGImageRelease(img);
    if(!ok) return NULL;
    wchar_t wtmp[PATH_MAX];
    mbstowcs(wtmp, tmp, PATH_MAX);
    return wcsdup(wtmp);
}

static void fs_to_wide(wchar_t* dst, const char* src, size_t dstlen){
    if(!dstlen) return;
    CFStringRef cf = CFStringCreateWithFileSystemRepresentation(NULL, src);
    if(!cf){ dst[0] = 0; return; }
    CFRange range = {0, CFStringGetLength(cf)};
#if __BIG_ENDIAN__
    CFStringEncoding enc = kCFStringEncodingUTF32BE;
#else
    CFStringEncoding enc = kCFStringEncodingUTF32LE;
#endif
    CFIndex used = 0;
    CFStringGetBytes(cf, range, enc, 0, false, (UInt8*)dst,
                     (dstlen - 1) * sizeof(wchar_t), &used);
    CFRelease(cf);
    size_t chars = (size_t)used / sizeof(wchar_t);
    dst[chars] = 0;
}

static BOOL has_ext(const char* path, const char* ext){
    const char* dot = strrchr(path, '.');
    if(!dot) return FALSE;
    return strcasecmp(dot+1, ext) == 0;
}

static void emit_zip_entries(FileScanner* s, const char* path){
    int err = 0;
    zip_t* z = zip_open(path, 0, &err);
    if(!z) return;
    zip_int64_t count = zip_get_num_entries(z, 0);
    for(zip_int64_t i=0;i<count;i++){
        const char* name = zip_get_name(z, i, 0);
        if(!name) continue;
        DbWorkItem* wi;
        if(posix_memalign((void**)&wi, CACHE_LINE_SIZE, sizeof(DbWorkItem))!=0) continue;
        wi->content = NULL;
        wi->preview = NULL;
        fs_to_wide(wi->parent_path, path, MAX_LONG_PATH);
        fs_to_wide(wi->name, name, MAX_PATH);
        wi->file_size = wi->creation_time = wi->modified_time = wi->access_time = 0;
        wi->attributes = 0;
        wi->clone_id = 0;
        wi->stage = INDEX_NAMES_ONLY;
        wi->op = WI_ADD;
        while(!MPMC_Push(s->outq, wi)) sched_yield();
    }
    zip_close(z);
}

static void emit_pst_tree(FileScanner* s, const wchar_t* parent, pst_file* pf, pst_desc_tree* node){
    for(pst_desc_tree* cur=node; cur; cur=cur->next){
        pst_item* item = pst_parse_item(pf, cur, NULL);
        if(item){
            pst_convert_utf8_null(item, &item->subject);
            const char* subj = item->subject ? item->subject : "";
            DbWorkItem* wi;
            if(posix_memalign((void**)&wi, CACHE_LINE_SIZE, sizeof(DbWorkItem))==0){
                wi->content = NULL;
                wi->preview = NULL;
                wcsncpy(wi->parent_path, parent, MAX_LONG_PATH);
                wi->parent_path[MAX_LONG_PATH-1] = 0;
                fs_to_wide(wi->name, subj, MAX_PATH);
                wi->file_size = wi->creation_time = wi->modified_time = wi->access_time = 0;
                wi->attributes = 0;
                wi->clone_id = 0;
                wi->stage = INDEX_NAMES_ONLY;
                wi->op = WI_ADD;
                while(!MPMC_Push(s->outq, wi)) sched_yield();
            }
            pst_freeItem(item);
        }
        if(cur->child) emit_pst_tree(s, parent, pf, cur->child);
    }
}

static void emit_pst_entries(FileScanner* s, const char* path){
    pst_file pf; memset(&pf,0,sizeof(pf));
    if(pst_open(&pf, path, NULL)!=0) return;
    if(pst_load_index(&pf)!=0){ pst_close(&pf); return; }
    if(pst_load_extended_attributes(&pf)!=0){ pst_close(&pf); return; }
    pst_item* root = pst_parse_item(&pf, pf.d_head, NULL);
    if(!root){ pst_close(&pf); return; }
    pst_desc_tree* top = pst_getTopOfFolders(&pf, root);
    if(top && top->child){
        wchar_t parent[MAX_LONG_PATH];
        fs_to_wide(parent, path, MAX_LONG_PATH);
        emit_pst_tree(s, parent, &pf, top->child);
    }
    pst_freeItem(root);
    pst_close(&pf);
}

static void emit(FileScanner* s, const char* path, int base){
    struct stat st;
    if(stat(path,&st)!=0) return;
    char parent[PATH_MAX];
    const char* name;
    if(base >= 0){
        name = path + base;
        if(base > 0){
            size_t len = base - 1;
            if(len >= sizeof(parent)) len = sizeof(parent) - 1;
            memcpy(parent, path, len);
            parent[len] = 0;
        } else {
            parent[0] = 0;
        }
    } else {
        const char* p = strrchr(path,'/');
        if(p){
            size_t len = p - path;
            strncpy(parent, path, len);
            parent[len]=0;
            p++;
            name = p;
        } else {
            parent[0]=0;
            name = path;
        }
    }
    DbWorkItem* wi;
    if(posix_memalign((void**)&wi, CACHE_LINE_SIZE, sizeof(DbWorkItem))!=0) return;
    wi->content = NULL;
    wi->preview = NULL;
    fs_to_wide(wi->parent_path, parent, MAX_LONG_PATH);
    fs_to_wide(wi->name, name, MAX_PATH);
    wi->file_size = st.st_size;
#if defined(__APPLE__)
    wi->creation_time = st.st_birthtime;
#else
    wi->creation_time = st.st_mtime;
#endif
    wi->modified_time = st.st_mtime;
    wi->access_time = st.st_atime;
    wi->attributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : 0;
    uint64_t cid;
    if(apfs_cloneid(path, &cid)){
        wi->clone_id = cid;
        wi->attributes |= FILE_ATTRIBUTE_CLONE;
    } else {
        wi->clone_id = 0;
    }
    wi->stage = INDEX_METADATA_LIGHT;
    wi->op = WI_ADD;
    while(!MPMC_Push(s->outq, wi)) sched_yield();
    if(!S_ISDIR(st.st_mode)){
        if(has_ext(path, "zip")) emit_zip_entries(s, path);
        else if(has_ext(path, "pst")) emit_pst_entries(s, path);
    }
}

static int enum_cb(const char* fpath, const struct stat* sb, int typeflag, struct FTW* ftwbuf){
    (void)sb; (void)typeflag;
    FileScanner* s = g_current;
    if(!s) return 0;
    if(is_cancelled(s->cancel)) s->stop = TRUE;
    if(s->stop) return 1;
    emit(s, fpath, ftwbuf ? ftwbuf->base : -1);
    return 0;
}

static void snapshot_cb(const char* name, void* ctx){
    FileScanner* s = (FileScanner*)ctx;
    char snap_path[PATH_MAX];
    snprintf(snap_path, sizeof(snap_path), "%s/.snapshots/%s", s->root, name);
    nftw(snap_path, enum_cb, 16, FTW_PHYS);
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
        if(eventFlags[i] & (kFSEventStreamEventFlagItemRemoved | kFSEventStreamEventFlagItemRenamed)){
            DbWorkItem* wi;
            if(posix_memalign((void**)&wi, CACHE_LINE_SIZE, sizeof(DbWorkItem))!=0) continue;
            wi->content = NULL;
            wi->preview = NULL;
            const char* p = strrchr(paths[i], '/');
            if(p){
                char parent[PATH_MAX]; size_t len = p - paths[i];
                strncpy(parent, paths[i], len); parent[len]=0;
                fs_to_wide(wi->parent_path, parent, MAX_LONG_PATH);
                fs_to_wide(wi->name, p+1, MAX_PATH);
            } else {
                fs_to_wide(wi->parent_path, "", MAX_LONG_PATH);
                fs_to_wide(wi->name, paths[i], MAX_PATH);
            }
            wi->file_size = wi->creation_time = wi->modified_time = wi->access_time = 0;
            wi->attributes = 0;
            wi->clone_id = 0;
            wi->stage = INDEX_NAMES_ONLY;
            wi->op = WI_DELETE;
            while(!MPMC_Push(s->outq, wi)) sched_yield();
        } else {
            emit(s, paths[i], -1);
        }
    }
}

static void* enum_thread(void* arg){
    FileScanner* s = (FileScanner*)arg;
    g_current = s;
    nftw(s->root, enum_cb, 16, FTW_PHYS);
    apfs_list_snapshots(s->root, snapshot_cb, s);
    return NULL;
}

static void* loop_thread(void* arg){
    FileScanner* s = (FileScanner*)arg;
    s->run_loop = CFRunLoopGetCurrent();
    FSEventStreamScheduleWithRunLoop(s->stream, s->run_loop, kCFRunLoopDefaultMode);
    FSEventStreamStart(s->stream);
    while(!s->stop){
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, TRUE);
    }
    return NULL;
}

FileScanner* FileScanner_Start(const wchar_t* rootPath, int threads, MPMCQueue* outQueue, CancelToken* cancelToken){
    (void)threads;
    char tmp[PATH_MAX];
    wcstombs(tmp, rootPath, PATH_MAX);
    struct statfs sfs;
    if(statfs(tmp, &sfs)==0){
        if(!(sfs.f_flags & MNT_LOCAL)){
            FileScanner* s = (FileScanner*)calloc(1, sizeof(FileScanner));
            if(!s) return NULL;
            s->is_network = TRUE;
            s->net = NetworkScanner_Start(rootPath, threads, outQueue, cancelToken);
            if(!s->net){ free(s); return NULL; }
            return s;
        }
    }
    FileScanner* s = (FileScanner*)calloc(1,sizeof(FileScanner));
    if(!s) return NULL;
    s->outq = outQueue;
    s->cancel = cancelToken;
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
    pthread_create(&s->loop_thread, NULL, loop_thread, s);
    pthread_create(&s->enum_thread, NULL, enum_thread, s);
    return s;
}

void FileScanner_Wait(FileScanner* s){
    if(!s) return;
    if(s->is_network){
        NetworkScanner_Wait(s->net);
    } else {
        pthread_join(s->enum_thread, NULL);
        s->enum_thread = 0;
    }
}

void FileScanner_Free(FileScanner* s){
    if(!s) return;
    if(s->is_network){
        NetworkScanner_Free(s->net);
        free(s);
        return;
    }
    s->stop = TRUE;
    CFRunLoopStop(s->run_loop);
    if(s->enum_thread) pthread_join(s->enum_thread, NULL);
    pthread_join(s->loop_thread, NULL);
    FSEventStreamStop(s->stream);
    FSEventStreamInvalidate(s->stream);
    FSEventStreamRelease(s->stream);
    free(s);
}

#endif
