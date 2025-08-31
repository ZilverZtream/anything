#ifdef _WIN32
#include <stdlib.h>
#include <shlwapi.h>
#include "scanner.h"

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
#endif
