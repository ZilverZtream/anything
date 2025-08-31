#include "plugin.h"
#include <tesseract/capi.h>
#include <wchar.h>
#include <stdlib.h>
#include <stdio.h>

static PluginHost g_host;

static BOOL init(const PluginHost* host){
    g_host = *host;
    return TRUE;
}

static wchar_t* ocr_file(const wchar_t* path){
    TessBaseAPI* api = TessBaseAPICreate();
    if(!api) return NULL;
    if(TessBaseAPIInit3(api, NULL, "eng")!=0){ TessBaseAPIDelete(api); return NULL; }
    char upath[MAX_PATH*4];
    WideCharToMultiByte(CP_UTF8,0,path,-1,upath,sizeof(upath),NULL,NULL);
    if(!TessBaseAPIProcessPages(api, upath, NULL, 0)){ TessBaseAPIDelete(api); return NULL; }
    char* text = TessBaseAPIGetUTF8Text(api);
    TessBaseAPIDelete(api);
    if(!text) return NULL;
    int wlen = MultiByteToWideChar(CP_UTF8,0,text,-1,NULL,0);
    wchar_t* out = (wchar_t*)malloc(sizeof(wchar_t)*wlen);
    if(out) MultiByteToWideChar(CP_UTF8,0,text,-1,out,wlen);
    TessDeleteText(text);
    return out;
}

static void scan(void){
    const wchar_t* root = L"ocr"; // folder to scan
    wchar_t pattern[MAX_PATH];
    _snwprintf(pattern, MAX_PATH, L"%s\\*.*", root);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pattern, &fd);
    if(h==INVALID_HANDLE_VALUE) return;
    do{
        if(WaitForSingleObject(g_host.cancel_event,0)==WAIT_OBJECT_0) break;
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
        DbWorkItem* wi = (DbWorkItem*)_aligned_malloc(sizeof(DbWorkItem), CACHE_LINE_SIZE);
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
        wi->stage = INDEX_FULL_CONTENT;
        wi->op = WI_ADD;
        int tries = 0;
        while(!MPMC_Push(g_host.queue, wi)){
            if(WaitForSingleObject(g_host.cancel_event,0)==WAIT_OBJECT_0 || tries++>1000){
                free(wi->content);
                _aligned_free(wi);
                FindClose(h);
                return;
            }
            Sleep(0);
        }
    }while(FindNextFileW(h,&fd));
    FindClose(h);
}

static void shutdown(void){
}

static AnythingPlugin g_plugin = {
    ANYTHING_PLUGIN_API_VERSION,
    L"OCR Plugin",
    init,
    scan,
    shutdown
};

__declspec(dllexport) AnythingPlugin* Anything_GetPlugin(void){
    return &g_plugin;
}
