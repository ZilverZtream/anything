#include "anything/plugin.h"
#include "anything/util.h"
#include <tesseract/capi.h>
#include <wchar.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#define _snwprintf swprintf
#define _wcsicmp wcscasecmp
#define Sleep(ms) usleep((ms)*1000)
static int wcscpy_s(wchar_t* dst,size_t dstcch,const wchar_t* src){ if(!dst||!src||dstcch==0) return 1; wcsncpy(dst,src,dstcch); dst[dstcch-1]=0; return 0; }
static uint64_t to_filetime(time_t t){ return ((uint64_t)t*10000000ULL)+116444736000000000ULL; }
#endif

static PluginHost g_host;

static BOOL init(const PluginHost* host){
    g_host = *host;
    return TRUE;
}

static wchar_t* ocr_file(const wchar_t* path){
    TessBaseAPI* api = TessBaseAPICreate();
    if(!api){
        fwprintf(stderr,L"[ocr] TessBaseAPICreate failed for %ls\n",path);
        return NULL;
    }
    if(TessBaseAPIInit3(api, NULL, "eng")!=0){
        fwprintf(stderr,L"[ocr] TessBaseAPIInit3 failed for %ls\n",path);
        TessBaseAPIDelete(api);
        return NULL;
    }
    char upath[MAX_PATH*4];
#ifdef _WIN32
    WideCharToMultiByte(CP_UTF8,0,path,-1,upath,sizeof(upath),NULL,NULL);
#else
    wcstombs(upath, path, sizeof(upath));
#endif
    if(!TessBaseAPIProcessPages(api, upath, NULL, 0)){
        fwprintf(stderr,L"[ocr] ProcessPages failed for %ls\n",path);
        TessBaseAPIDelete(api);
        return NULL;
    }
    char* text = TessBaseAPIGetUTF8Text(api);
    TessBaseAPIDelete(api);
    if(!text){
        fwprintf(stderr,L"[ocr] GetUTF8Text returned NULL for %ls\n",path);
        return NULL;
    }
#ifdef _WIN32
    int wlen = MultiByteToWideChar(CP_UTF8,0,text,-1,NULL,0);
    wchar_t* out = (wchar_t*)malloc(sizeof(wchar_t)*wlen);
    if(out) MultiByteToWideChar(CP_UTF8,0,text,-1,out,wlen);
#else
    size_t wlen = mbstowcs(NULL, text, 0);
    wchar_t* out = (wchar_t*)malloc(sizeof(wchar_t)*(wlen+1));
    if(out) mbstowcs(out, text, wlen+1);
#endif
    TessDeleteText(text);
    return out;
}

#ifdef _WIN32
static void scan(void){
    const wchar_t* root = L"ocr"; // folder to scan
    wchar_t pattern[MAX_PATH];
    _snwprintf(pattern, MAX_PATH, L"%s\\*.*", root);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pattern, &fd);
    if(h==INVALID_HANDLE_VALUE){
        fwprintf(stderr,L"[ocr] failed to open %ls: %lu\n",root,GetLastError());
        return;
    }
    do{
        if(is_cancelled(g_host.cancel_token)) break;
        if(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const wchar_t* ext = wcsrchr(fd.cFileName, L'.');
        if(!ext) continue; ext++;
        if(_wcsicmp(ext,L"png") && _wcsicmp(ext,L"jpg") && _wcsicmp(ext,L"jpeg") &&
           _wcsicmp(ext,L"bmp") && _wcsicmp(ext,L"tif") && _wcsicmp(ext,L"tiff") &&
           _wcsicmp(ext,L"pdf")) continue;
        wchar_t full[MAX_PATH];
        _snwprintf(full, MAX_PATH, L"%s\\%s", root, fd.cFileName);
        wchar_t* text = ocr_file(full);
        if(!text) continue;
        DbWorkItem* wi = (DbWorkItem*)aligned_malloc(sizeof(DbWorkItem), CACHE_LINE_SIZE);
        if(!wi){ free(text); continue; }
        wi->content = text;
        wi->preview = NULL;
        wcscpy_s(wi->parent_path, MAX_LONG_PATH, root);
        wcscpy_s(wi->name, MAX_PATH, fd.cFileName);
        ULARGE_INTEGER s; s.LowPart=fd.nFileSizeLow; s.HighPart=fd.nFileSizeHigh; wi->file_size=s.QuadPart;
        wi->creation_time = ((ULARGE_INTEGER){fd.ftCreationTime.dwLowDateTime, fd.ftCreationTime.dwHighDateTime}).QuadPart;
        wi->modified_time = ((ULARGE_INTEGER){fd.ftLastWriteTime.dwLowDateTime, fd.ftLastWriteTime.dwHighDateTime}).QuadPart;
        wi->access_time   = ((ULARGE_INTEGER){fd.ftLastAccessTime.dwLowDateTime, fd.ftLastAccessTime.dwHighDateTime}).QuadPart;
        wi->attributes = fd.dwFileAttributes;
        wi->clone_id = 0;
        wi->hash_crc = 0;
        wi->hash_ready = FALSE;
        wi->stage = INDEX_FULL_CONTENT;
        wi->op = WI_ADD;
        int tries = 0;
        while(!MPMC_Push(g_host.queue, wi)){
            if(is_cancelled(g_host.cancel_token) || tries++>1000){
                free(wi->content);
                aligned_free(wi);
                FindClose(h);
                return;
            }
            Sleep(0);
        }
    }while(FindNextFileW(h,&fd));
    FindClose(h);
}
#else
static void scan(void){
    const wchar_t* root = L"ocr"; // folder to scan
    char root_mb[PATH_MAX];
    wcstombs(root_mb, root, sizeof(root_mb));
    DIR* d = opendir(root_mb);
    if(!d){
        fprintf(stderr,"[ocr] failed to open %s: %s\n", root_mb, strerror(errno));
        return;
    }
    struct dirent* ent;
    while((ent = readdir(d))){
        if(is_cancelled(g_host.cancel_token)) break;
        char* ext = strrchr(ent->d_name, '.');
        if(!ext) continue; ext++;
        wchar_t wext[32]; mbstowcs(wext, ext, 32);
        if(_wcsicmp(wext,L"png") && _wcsicmp(wext,L"jpg") && _wcsicmp(wext,L"jpeg") &&
           _wcsicmp(wext,L"bmp") && _wcsicmp(wext,L"tif") && _wcsicmp(wext,L"tiff") &&
           _wcsicmp(wext,L"pdf")) continue;
        char full_mb[PATH_MAX];
        snprintf(full_mb, sizeof(full_mb), "%s/%s", root_mb, ent->d_name);
        struct stat st; if(stat(full_mb,&st)!=0){
            fprintf(stderr,"[ocr] stat failed %s: %s\n", full_mb, strerror(errno));
            continue;
        }
        if(S_ISDIR(st.st_mode)) continue;
        wchar_t full[MAX_PATH]; mbstowcs(full, full_mb, MAX_PATH);
        for(wchar_t* p=full; *p; ++p) if(*p==L'/') *p=L'\\';
        wchar_t* text = ocr_file(full);
        if(!text) continue;
        DbWorkItem* wi = (DbWorkItem*)aligned_malloc(sizeof(DbWorkItem), CACHE_LINE_SIZE);
        if(!wi){ free(text); continue; }
        wi->content = text;
        wi->preview = NULL;
        wcscpy_s(wi->parent_path, MAX_LONG_PATH, root);
        wchar_t name_w[MAX_PATH]; mbstowcs(name_w, ent->d_name, MAX_PATH);
        for(wchar_t* p=name_w; *p; ++p) if(*p==L'/') *p=L'\\';
        wcscpy_s(wi->name, MAX_PATH, name_w);
        wi->file_size = (uint64_t)st.st_size;
        wi->creation_time = to_filetime(st.st_ctime);
        wi->modified_time = to_filetime(st.st_mtime);
        wi->access_time   = to_filetime(st.st_atime);
        wi->attributes = 0;
        wi->clone_id = 0;
        wi->hash_crc = 0;
        wi->hash_ready = FALSE;
        wi->stage = INDEX_FULL_CONTENT;
        wi->op = WI_ADD;
        int tries = 0;
        while(!MPMC_Push(g_host.queue, wi)){
            if(is_cancelled(g_host.cancel_token) || tries++>1000){
                free(wi->content);
                aligned_free(wi);
                closedir(d);
                return;
            }
            Sleep(0);
        }
    }
    closedir(d);
}
#endif

static void plugin_shutdown(void){
}

static AnythingPlugin g_plugin = {
    ANYTHING_PLUGIN_API_VERSION,
    L"OCR Plugin",
    init,
    scan,
    plugin_shutdown
};

__declspec(dllexport) AnythingPlugin* Anything_GetPlugin(void){
    return &g_plugin;
}
