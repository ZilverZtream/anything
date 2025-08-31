// Duplicate Finder Plugin
// Scans a fixed directory tree and pushes work items for files that have
// duplicate content (same size and CRC64 hash) onto the host queue.

#include "plugin.h"
#include "util.h"

#include <windows.h>
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

static BOOL init(const PluginHost* host){
    if(!host) return FALSE;
    g_host = *host;
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

static void scan_dir(const wchar_t* dir){
    if(WaitForSingleObject(g_host.cancel_event,0)==WAIT_OBJECT_0) return;
    wchar_t pattern[MAX_LONG_PATH];
    _snwprintf(pattern, MAX_LONG_PATH, L"%s\\*.*", dir);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pattern,&fd);
    if(h==INVALID_HANDLE_VALUE) return;
    do{
        if(WaitForSingleObject(g_host.cancel_event,0)==WAIT_OBJECT_0) break;
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
            fe.hash  = crc64_file(full);
            add_file(&fe);
        }
    }while(FindNextFileW(h,&fd));
    FindClose(h);
}

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
    qsort(g_files, g_file_count, sizeof(FileEntry), cmp_file);
    size_t i = 0;
    while(i < g_file_count){
        size_t j = i+1;
        while(j < g_file_count &&
              g_files[j].size == g_files[i].size &&
              g_files[j].hash == g_files[i].hash) j++;
        if(j - i > 1){
            for(size_t k=i; k<j; k++){
                DbWorkItem* wi = (DbWorkItem*)_aligned_malloc(sizeof(DbWorkItem), CACHE_LINE_SIZE);
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
                while(!MPMC_Push(g_host.queue, wi)){
                    if(WaitForSingleObject(g_host.cancel_event,0)==WAIT_OBJECT_0){
                        _aligned_free(wi);
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
    const wchar_t* root = L"duplicates"; // directory to scan
    scan_dir(root);
    emit_duplicates();
    free_state();
}

static void plugin_shutdown(void){
    free_state();
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
