#ifndef SCANNER_H
#define SCANNER_H

#include "anything.h"

#ifdef _WIN32
typedef struct FileScanner FileScanner;
FileScanner* FileScanner_Start(const wchar_t* rootPath, int threads, MPMCQueue* outQueue, HANDLE cancelEvent);
void FileScanner_Wait(FileScanner* s);
void FileScanner_Free(FileScanner* s);
HANDLE StartUSNTailer(const wchar_t* volumeRoot, MPMCQueue* outQueue, HANDLE cancelEvent);
wchar_t* GenerateThumbnail(const wchar_t* path);
#elif defined(__linux__)
typedef struct FileScanner FileScanner;
FileScanner* FileScanner_Start(const wchar_t* rootPath, int threads, MPMCQueue* outQueue, HANDLE cancelEvent);
void FileScanner_Wait(FileScanner* s);
void FileScanner_Free(FileScanner* s);
static inline wchar_t* GenerateThumbnail(const wchar_t* path){ (void)path; return NULL; }
#elif defined(__APPLE__)
typedef struct FileScanner FileScanner;
FileScanner* FileScanner_Start(const wchar_t* rootPath, int threads, MPMCQueue* outQueue, HANDLE cancelEvent);
void FileScanner_Wait(FileScanner* s);
void FileScanner_Free(FileScanner* s);
wchar_t* GenerateThumbnail(const wchar_t* path);
#endif

#endif
