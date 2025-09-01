#ifdef _WIN32
#include <stdlib.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <gdiplus.h>
#include "scanner.h"
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

FileScanner* FileScanner_Start(const wchar_t* rootPath, int threads, MPMCQueue* outQueue, HANDLE cancelEvent){
    FileScanner* s = (FileScanner*)calloc(1, sizeof(FileScanner));
    if(!s) return NULL;
    if(PathIsUNCW(rootPath) || PathIsNetworkPathW(rootPath)){
        s->u.net = NetworkScanner_Start(rootPath, threads, outQueue, cancelEvent);
        if(!s->u.net){ free(s); return NULL; }
        s->kind = FS_NETWORK;
        return s;
    }
    s->u.ntfs = NTFSScanner_Start(rootPath, threads, outQueue, cancelEvent);
    if(s->u.ntfs){
        s->kind = FS_NTFS;
        return s;
    }
    s->u.gen = GenericScanner_Start(rootPath, threads, outQueue, cancelEvent);
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
    CoInitialize(NULL);
    IShellItemImageFactory* factory = NULL;
    if(FAILED(SHCreateItemFromParsingName(path, NULL, &IID_IShellItemImageFactory, (void**)&factory))){
        CoUninitialize();
        return NULL;
    }
    SIZE sz = {256,256};
    HBITMAP hbmp;
    HRESULT hr = factory->lpVtbl->GetImage(factory, sz, SIIGBF_BIGGERSIZEOK | SIIGBF_RESIZETOFIT, &hbmp);
    factory->lpVtbl->Release(factory);
    if(FAILED(hr)){ CoUninitialize(); return NULL; }
    wchar_t tmp[MAX_PATH];
    if(!GetTempFileNameW(L"", L"ath", 0, tmp)){
        DeleteObject(hbmp);
        CoUninitialize();
        return NULL;
    }
    DeleteFileW(tmp);
    wcscat_s(tmp, MAX_PATH, L".png");
    if(!save_hbitmap_png(hbmp, tmp)){
        DeleteObject(hbmp);
        CoUninitialize();
        return NULL;
    }
    DeleteObject(hbmp);
    CoUninitialize();
    return _wcsdup(tmp);
}
#endif
