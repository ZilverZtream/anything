#ifdef _WIN32
#include <stdlib.h>
#include "scanner.h"

struct FileScanner {
    BOOL is_ntfs;
    union {
        NTFSScanner* ntfs;
        GenericScanner* gen;
    } u;
};

FileScanner* FileScanner_Start(const wchar_t* rootPath, int threads, MPMCQueue* outQueue, HANDLE cancelEvent){
    FileScanner* s = (FileScanner*)calloc(1, sizeof(FileScanner));
    if(!s) return NULL;
    s->u.ntfs = NTFSScanner_Start(rootPath, threads, outQueue, cancelEvent);
    if(s->u.ntfs){
        s->is_ntfs = TRUE;
        return s;
    }
    s->u.gen = GenericScanner_Start(rootPath, threads, outQueue, cancelEvent);
    if(!s->u.gen){
        free(s);
        return NULL;
    }
    return s;
}

void FileScanner_Wait(FileScanner* s){
    if(!s) return;
    if(s->is_ntfs) NTFSScanner_Wait(s->u.ntfs);
    else GenericScanner_Wait(s->u.gen);
}

void FileScanner_Free(FileScanner* s){
    if(!s) return;
    if(s->is_ntfs) NTFSScanner_Free(s->u.ntfs);
    else GenericScanner_Free(s->u.gen);
    free(s);
}
#endif
