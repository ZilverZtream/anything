// Duplicate Finder Plugin
// Scans a fixed directory tree and pushes work items for files that have
// duplicate content (same size and CRC64 hash) onto the host queue.

#include "anything/plugin.h"
#include "anything/util.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <pthread.h>
#define _snwprintf swprintf
#define Sleep(ms) usleep((ms)*1000)
static int wcscpy_s(wchar_t* dst, size_t dstcch, const wchar_t* src){
    if(!dst || !src || dstcch==0) return 1;
    wcsncpy(dst, src, dstcch);
    dst[dstcch-1] = 0;
    return 0;
}
static uint64_t to_filetime(time_t t){ return ((uint64_t)t*10000000ULL)+116444736000000000ULL; }
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>

static PluginHost g_host;

typedef struct FileEntry {
    wchar_t  path[MAX_LONG_PATH];
    uint64_t size;
    uint64_t hash;
    uint32_t attrs;
    uint64_t ctime;
    uint64_t mtime;
    uint64_t atime;
} FileEntry;

static FileEntry* g_files = NULL;
static size_t     g_file_count = 0;
static size_t     g_file_cap   = 0;

#ifdef _WIN32
static HANDLE g_mutex;
#else
static pthread_mutex_t g_mutex;
#endif

static BOOL init(const PluginHost* host){
    if(!host) return FALSE;
    g_host = *host;
#ifdef _WIN32
    if(!g_mutex) g_mutex = CreateMutex(NULL, FALSE, NULL);
#else
    pthread_mutex_init(&g_mutex, NULL);
#endif
    return TRUE;
}

static void add_file(const FileEntry* fe){
    if(g_file_count >= g_file_cap){
        size_t new_cap = g_file_cap ? g_file_cap * 2 : 1024;
        FileEntry* tmp = (FileEntry*)realloc(g_files, new_cap * sizeof(FileEntry));
        if(!tmp) return;
        g_files = tmp; g_file_cap = new_cap;
    }
    g_files[g_file_count++] = *fe;
}

#ifdef _WIN32
static void scan_dir(const wchar_t* dir){
    if(is_cancelled(g_host.cancel_token)) return;
    wchar_t pattern[MAX_LONG_PATH];
    _snwprintf(pattern, MAX_LONG_PATH, L"%s\\*.*", dir);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pattern,&fd);
    if(h==INVALID_HANDLE_VALUE){
        fwprintf(stderr,L"[duplicates] failed to open %ls: %lu\n",dir,GetLastError());
        return;
    }
    do{
        if(is_cancelled(g_host.cancel_token)) break;
        if(fd.cFileName[0]==L'.' && (fd.cFileName[1]==0 || (fd.cFileName[1]==L'.' && fd.cFileName[2]==0))) continue;
        wchar_t full[MAX_LONG_PATH];
        _snwprintf(full, MAX_LONG_PATH, L"%s\\%s", dir, fd.cFileName);
        if(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY){
            scan_dir(full);
        }else{
            FileEntry fe;
            wcscpy_s(fe.path, MAX_LONG_PATH, full);
            ULARGE_INTEGER s; s.LowPart = fd.nFileSizeLow; s.HighPart = fd.nFileSizeHigh;
            fe.size = s.QuadPart;
            fe.attrs = fd.dwFileAttributes;
            fe.ctime = ((ULARGE_INTEGER){fd.ftCreationTime.dwLowDateTime, fd.ftCreationTime.dwHighDateTime}).QuadPart;
            fe.mtime = ((ULARGE_INTEGER){fd.ftLastWriteTime.dwLowDateTime, fd.ftLastWriteTime.dwHighDateTime}).QuadPart;
            fe.atime = ((ULARGE_INTEGER){fd.ftLastAccessTime.dwLowDateTime, fd.ftLastAccessTime.dwHighDateTime}).QuadPart;
            BOOL hash_ok = FALSE;
            fe.hash  = crc64_file(full, g_host.cancel_token, NULL, NULL, &hash_ok);
            if(hash_ok){
                add_file(&fe);
            }
        }
    }while(FindNextFileW(h,&fd));
    FindClose(h);
}
#else
static void scan_dir(const wchar_t* dir){
    if(is_cancelled(g_host.cancel_token)) return;
    char dir_mb[PATH_MAX];
    wcstombs(dir_mb, dir, sizeof(dir_mb));
    DIR* d = opendir(dir_mb);
    if(!d){
        fprintf(stderr,"[duplicates] failed to open %s: %s\n", dir_mb, strerror(errno));
        return;
    }
    struct dirent* ent;
    while((ent = readdir(d))){
        if(is_cancelled(g_host.cancel_token)) break;
        if(ent->d_name[0]=='.' && (ent->d_name[1]==0 || (ent->d_name[1]=='.' && ent->d_name[2]==0))) continue;
        char full_mb[PATH_MAX];
        snprintf(full_mb, sizeof(full_mb), "%s/%s", dir_mb, ent->d_name);
        struct stat st;
        if(stat(full_mb, &st)!=0){
            fprintf(stderr,"[duplicates] stat failed %s: %s\n", full_mb, strerror(errno));
            continue;
        }
        wchar_t full[MAX_LONG_PATH];
        mbstowcs(full, full_mb, MAX_LONG_PATH);
        for(wchar_t* p=full; *p; ++p) if(*p==L'/') *p=L'\\';
        if(S_ISDIR(st.st_mode)){
            scan_dir(full);
        }else{
            FileEntry fe;
            wcscpy_s(fe.path, MAX_LONG_PATH, full);
            fe.size = (uint64_t)st.st_size;
            fe.attrs = 0;
            fe.ctime = to_filetime(st.st_ctime);
            fe.mtime = to_filetime(st.st_mtime);
            fe.atime = to_filetime(st.st_atime);
            BOOL hash_ok = FALSE;
            fe.hash  = crc64_file(full, g_host.cancel_token, NULL, NULL, &hash_ok);
            if(hash_ok){
                add_file(&fe);
            }
        }
    }
    closedir(d);
}
#endif

static int cmp_file(const void* a, const void* b){
    const FileEntry* fa = (const FileEntry*)a;
    const FileEntry* fb = (const FileEntry*)b;
    if(fa->size < fb->size) return -1;
    if(fa->size > fb->size) return 1;
    if(fa->hash < fb->hash) return -1;
    if(fa->hash > fb->hash) return 1;
    return wcscmp(fa->path, fb->path);
}

static void emit_duplicates(void){
    if(g_file_count==0) return;

    // Use external_sort instead of qsort to avoid loading entire file list into memory
    // For large drives with millions of files, this prevents OOM crashes
    if(!external_sort(NULL, g_files, g_file_count, sizeof(FileEntry), cmp_file)){
        fprintf(stderr, "[duplicates] external_sort failed, falling back to qsort\n");
        qsort(g_files, g_file_count, sizeof(FileEntry), cmp_file);
    }
    size_t i = 0;
    while(i < g_file_count){
        size_t j = i+1;
        while(j < g_file_count &&
              g_files[j].size == g_files[i].size &&
              g_files[j].hash == g_files[i].hash) j++;
        if(j - i > 1){
            for(size_t k=i; k<j; k++){
                DbWorkItem* wi = (DbWorkItem*)aligned_malloc(sizeof(DbWorkItem), CACHE_LINE_SIZE);
                if(!wi) continue;
                const wchar_t* path = g_files[k].path;
                wchar_t parent[MAX_LONG_PATH];
                path_dirname(path,parent,MAX_LONG_PATH);
                const wchar_t* name = wcsrchr(path, L'\\');
                if(name) name++; else name = path;
                wcscpy_s(wi->parent_path, MAX_LONG_PATH, parent);
                wcscpy_s(wi->name, MAX_PATH, name);
                wi->file_size     = g_files[k].size;
                wi->creation_time = g_files[k].ctime;
                wi->modified_time = g_files[k].mtime;
                wi->access_time   = g_files[k].atime;
                wi->attributes    = g_files[k].attrs;
                wi->clone_id      = 0;
                wi->stage         = INDEX_METADATA_LIGHT;
                wi->op            = WI_ADD;
                wi->content = NULL;
                wi->preview = NULL;
                wi->hash_crc      = g_files[k].hash;
                wi->hash_ready    = TRUE;
                while(!MPMC_Push(g_host.queue, wi)){
                    if(is_cancelled(g_host.cancel_token)){
                        aligned_free(wi);
                        return;
                    }
                    Sleep(0);
                }
            }
        }
        i = j;
    }
}

static void free_state(void){
    free(g_files); g_files=NULL; g_file_count=0; g_file_cap=0;
}

static void scan(void){
#ifdef _WIN32
    WaitForSingleObject(g_mutex, INFINITE);
#else
    pthread_mutex_lock(&g_mutex);
#endif

    const wchar_t* root = L"duplicates"; // directory to scan
    scan_dir(root);
    emit_duplicates();
    free_state();

#ifdef _WIN32
    ReleaseMutex(g_mutex);
#else
    pthread_mutex_unlock(&g_mutex);
#endif
}

static void plugin_shutdown(void){
#ifdef _WIN32
    WaitForSingleObject(g_mutex, INFINITE);
#else
    pthread_mutex_lock(&g_mutex);
#endif
    free_state();
#ifdef _WIN32
    ReleaseMutex(g_mutex);
    CloseHandle(g_mutex);
    g_mutex = NULL;
#else
    pthread_mutex_unlock(&g_mutex);
    pthread_mutex_destroy(&g_mutex);
#endif
}

static AnythingPlugin g_plugin = {
    ANYTHING_PLUGIN_API_VERSION,
    L"Duplicate Finder Plugin",
    init,
    scan,
    plugin_shutdown
};

__declspec(dllexport) AnythingPlugin* Anything_GetPlugin(void){
    return &g_plugin;
}
