#ifndef SCANNER_H
#define SCANNER_H

#include "anything.h"

typedef struct FileScanner FileScanner;

FileScanner* FileScanner_Start(const wchar_t* rootPath, int threads, MPMCQueue* outQueue, HANDLE cancelEvent);
void FileScanner_Wait(FileScanner* s);
void FileScanner_Free(FileScanner* s);

#endif
