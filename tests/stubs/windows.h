#ifndef WINDOWS_H
#define WINDOWS_H
#include <stdint.h>
#include <stdlib.h>
#include <wchar.h>
#include <stdarg.h>
#include <alloca.h>
#include <string.h>

typedef int BOOL;
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned int UINT;
typedef uint32_t DWORD;
typedef int64_t LONG;
typedef int64_t LONG64;
typedef uint64_t ULONGLONG;
typedef uint64_t ULONG64;
typedef void* HANDLE;
typedef BYTE* PUCHAR;
typedef unsigned long ULONG;
#define CP_UTF8 65001
#define GetFileExInfoStandard 0
void* VirtualAlloc(void*, size_t, DWORD, DWORD);
BOOL VirtualFree(void*, size_t, DWORD);
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define PAGE_READWRITE 0x04
#define MEM_RELEASE 0x8000

#define TRUE 1
#define FALSE 0
#define MAX_PATH 260
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)(-1))
#define GENERIC_READ 0
#define GENERIC_WRITE 0
#define FILE_SHARE_READ 0
#define FILE_SHARE_WRITE 0
#define FILE_SHARE_DELETE 0
#define OPEN_EXISTING 0
#define OPEN_ALWAYS 0
#define FILE_FLAG_SEQUENTIAL_SCAN 0
#define FILE_FLAG_DELETE_ON_CLOSE 0
#define FILE_ATTRIBUTE_TEMPORARY 0
#define FILE_ATTRIBUTE_NORMAL 0
#define CREATE_ALWAYS 0
#define FILE_BEGIN 0
#define _TRUNCATE ((size_t)-1)
#define FORMAT_MESSAGE_FROM_SYSTEM 0
#define FORMAT_MESSAGE_IGNORE_INSERTS 0

#define _malloca(size) alloca(size)
#define _freea(p) ((void)0)
#define ZeroMemory(p,s) memset((p),0,(s))

typedef struct _FILETIME {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
} FILETIME;

typedef struct _WIN32_FILE_ATTRIBUTE_DATA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastWriteTime;
    FILETIME ftLastAccessTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA;

typedef struct _MEMORYSTATUSEX {
    DWORD dwLength;
    DWORD dwMemoryLoad;
    ULONGLONG ullTotalPhys;
    ULONGLONG ullAvailPhys;
    ULONGLONG ullTotalPageFile;
    ULONGLONG ullAvailPageFile;
    ULONGLONG ullTotalVirtual;
    ULONGLONG ullAvailVirtual;
    ULONGLONG ullAvailExtendedVirtual;
} MEMORYSTATUSEX;

typedef union _ULARGE_INTEGER {
    struct {
        DWORD LowPart;
        DWORD HighPart;
    };
    uint64_t QuadPart;
} ULARGE_INTEGER;

typedef union _LARGE_INTEGER {
    struct {
        DWORD LowPart;
        LONG HighPart;
    };
    int64_t QuadPart;
} LARGE_INTEGER;

BOOL GlobalMemoryStatusEx(MEMORYSTATUSEX* ms);
int wcscpy_s(wchar_t* dst, size_t dstcch, const wchar_t* src);
int wcsncat_s(wchar_t* dst, size_t dstcch, const wchar_t* src, size_t count);
int swprintf_s(wchar_t* dst, size_t dstcch, const wchar_t* fmt, ...);
int sprintf_s(char* dst, size_t dstcch, const char* fmt, ...);

BOOL GetFileAttributesExW(const wchar_t* path, int infoLevel, WIN32_FILE_ATTRIBUTE_DATA* data);
BOOL GetVolumeInformationW(const wchar_t*, wchar_t*, DWORD, DWORD*, DWORD*, DWORD*, wchar_t*, DWORD);
BOOL GetDiskFreeSpaceExW(const wchar_t*, ULARGE_INTEGER*, ULARGE_INTEGER*, ULARGE_INTEGER*);
BOOL GetTempPathW(DWORD, wchar_t*);
BOOL GetTempFileNameW(const wchar_t*, const wchar_t*, DWORD, wchar_t*);
HANDLE CreateFileW(const wchar_t*, DWORD, DWORD, void*, DWORD, DWORD, HANDLE);
BOOL ReadFile(HANDLE, void*, DWORD, DWORD*, void*);
BOOL WriteFile(HANDLE, const void*, DWORD, DWORD*, void*);
BOOL CloseHandle(HANDLE);
DWORD SetFilePointer(HANDLE, LONG, void*, DWORD);
BOOL SetFilePointerEx(HANDLE, LARGE_INTEGER, LARGE_INTEGER*, DWORD);
BOOL GetFileSizeEx(HANDLE, LARGE_INTEGER*);
BOOL CreateDirectoryW(const wchar_t*, void*);
DWORD GetLastError(void);
void GetSystemTimeAsFileTime(FILETIME*);
BOOL FormatMessageA(DWORD, const void*, DWORD, DWORD, char*, DWORD, void*);
int wcsncpy_s(wchar_t* dst, size_t dstcch, const wchar_t* src, size_t count);
void* _aligned_malloc(size_t, size_t);
void _aligned_free(void*);
int WideCharToMultiByte(UINT, DWORD, const wchar_t*, int, char*, int, const char*, BOOL*);
int MultiByteToWideChar(UINT, DWORD, const char*, int, wchar_t*, int);

static inline unsigned char _BitScanForward(unsigned long* Index, unsigned long Mask){
    if(!Mask) return 0; *Index = __builtin_ctzl(Mask); return 1;
}

static inline void __cpuid(int info[4], int infoType){
    info[0]=info[1]=info[2]=info[3]=0;
}
static inline void __cpuidex(int info[4], int infoType, int ecx){
    info[0]=info[1]=info[2]=info[3]=0;
}

#endif
