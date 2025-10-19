#ifdef _WIN32
#include "core/pch.h"
#include <shobjidl.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

typedef enum { FS_NTFS, FS_GENERIC, FS_NETWORK } ScannerKind;

struct FileScanner {
    ScannerKind kind;
    union {
        NTFSScanner* ntfs;
        GenericScanner* gen;
        NetworkScanner* net;
    } u;
};

FileScanner* FileScanner_Start(const wchar_t* rootPath, int threads, MPMCQueue* outQueue, CancelToken* cancelToken){
    if(threads <= 0){
        SYSTEM_INFO si; GetSystemInfo(&si);
        threads = si.dwNumberOfProcessors;
    }
    FileScanner* s = (FileScanner*)calloc(1, sizeof(FileScanner));
    if(!s) return NULL;
    if(PathIsUNCW(rootPath) || PathIsNetworkPathW(rootPath)){
        s->u.net = NetworkScanner_Start(rootPath, threads, outQueue, cancelToken);
        if(!s->u.net){ free(s); return NULL; }
        s->kind = FS_NETWORK;
        return s;
    }
    s->u.ntfs = NTFSScanner_Start(rootPath, threads, outQueue, cancelToken);
    if(s->u.ntfs){
        s->kind = FS_NTFS;
        return s;
    }
    s->u.gen = GenericScanner_Start(rootPath, threads, outQueue, cancelToken);
    if(!s->u.gen){
        free(s);
        return NULL;
    }
    s->kind = FS_GENERIC;
    return s;
}

void FileScanner_Wait(FileScanner* s){
    if(!s) return;
    switch(s->kind){
    case FS_NTFS: NTFSScanner_Wait(s->u.ntfs); break;
    case FS_GENERIC: GenericScanner_Wait(s->u.gen); break;
    case FS_NETWORK: NetworkScanner_Wait(s->u.net); break;
    }
}

void FileScanner_Free(FileScanner* s){
    if(!s) return;
    switch(s->kind){
    case FS_NTFS: NTFSScanner_Free(s->u.ntfs); break;
    case FS_GENERIC: GenericScanner_Free(s->u.gen); break;
    case FS_NETWORK: NetworkScanner_Free(s->u.net); break;
    }
    free(s);
}

static INIT_ONCE g_comInitOnce = INIT_ONCE_STATIC_INIT;
static DWORD g_comTlsIndex = TLS_OUT_OF_INDEXES;
static CRITICAL_SECTION g_comInitLock;

static BOOL CALLBACK init_com_runtime(PINIT_ONCE initOnce, PVOID parameter, PVOID* context){
    (void)initOnce; (void)parameter; (void)context;
    InitializeCriticalSection(&g_comInitLock);
    g_comTlsIndex = TlsAlloc();
    return g_comTlsIndex != TLS_OUT_OF_INDEXES;
}

static BOOL ensure_thread_com_initialized(void){
    if(!InitOnceExecuteOnce(&g_comInitOnce, init_com_runtime, NULL, NULL)){
        return FALSE;
    }

    if(TlsGetValue(g_comTlsIndex)){
        return TRUE;
    }

    EnterCriticalSection(&g_comInitLock);

    if(TlsGetValue(g_comTlsIndex)){
        LeaveCriticalSection(&g_comInitLock);
        return TRUE;
    }

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if(hr == RPC_E_CHANGED_MODE){
        LeaveCriticalSection(&g_comInitLock);
        return FALSE;
    }
    if(FAILED(hr)){
        LeaveCriticalSection(&g_comInitLock);
        return FALSE;
    }

    BOOL release_on_exit = (hr == S_FALSE);

    if(!TlsSetValue(g_comTlsIndex, (LPVOID)1)){
        if(hr == S_OK || hr == S_FALSE){
            CoUninitialize();
        }
        LeaveCriticalSection(&g_comInitLock);
        return FALSE;
    }

    if(release_on_exit){
        CoUninitialize();
    }

    LeaveCriticalSection(&g_comInitLock);
    return TRUE;
}

static BOOL save_hbitmap_png(HBITMAP hbmp, const wchar_t* path){
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token;
    if(Gdiplus::GdiplusStartup(&token, &input, NULL) != Gdiplus::Ok) return FALSE;
    Gdiplus::Bitmap bmp(hbmp, NULL);
    UINT num=0, size=0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if(size==0){ Gdiplus::GdiplusShutdown(token); return FALSE; }
    Gdiplus::ImageCodecInfo* codecs = (Gdiplus::ImageCodecInfo*)malloc(size);
    if(!codecs){ Gdiplus::GdiplusShutdown(token); return FALSE; }
    Gdiplus::GetImageEncoders(num, size, codecs);
    CLSID pngClsid={0};
    for(UINT i=0;i<num;i++) if(wcscmp(codecs[i].MimeType, L"image/png")==0){ pngClsid = codecs[i].Clsid; break; }
    free(codecs);
    BOOL ok = bmp.Save(path, &pngClsid, NULL) == Gdiplus::Ok;
    Gdiplus::GdiplusShutdown(token);
    return ok;
}

wchar_t* GenerateThumbnail(const wchar_t* path){
    if(!ensure_thread_com_initialized()){
        return NULL;
    }
    IShellItemImageFactory* factory = NULL;
    if(FAILED(SHCreateItemFromParsingName(path, NULL, &IID_IShellItemImageFactory, (void**)&factory))){
        return NULL;
    }
    SIZE sz = {256,256};
    HBITMAP hbmp;
    HRESULT hr = factory->lpVtbl->GetImage(factory, sz, SIIGBF_BIGGERSIZEOK | SIIGBF_RESIZETOFIT, &hbmp);
    factory->lpVtbl->Release(factory);
    if(FAILED(hr)){
        return NULL;
    }
    wchar_t tmp[MAX_PATH];
    if(!GetTempFileNameW(L"", L"ath", 0, tmp)){
        DeleteObject(hbmp);
        return NULL;
    }
    DeleteFileW(tmp);
    wcscat_s(tmp, MAX_PATH, L".png");
    if(!save_hbitmap_png(hbmp, tmp)){
        DeleteObject(hbmp);
        return NULL;
    }
    DeleteObject(hbmp);
    return _wcsdup(tmp);
}
#endif
