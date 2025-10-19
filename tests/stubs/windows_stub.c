#include "windows_stub.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

BOOL GlobalMemoryStatusEx(MEMORYSTATUSEX* ms){ if(ms){ ms->ullAvailPhys = 0; } return 0; }
int wcscpy_s(wchar_t* dst, size_t dstcch, const wchar_t* src){ if(!dst||!src) return 1; wcsncpy(dst, src, dstcch); if(dstcch>0) dst[dstcch-1]=0; return 0; }
int wcsncpy_s(wchar_t* dst, size_t dstcch, const wchar_t* src, size_t count){
    if(!dst||!src||dstcch==0) return 1;
    size_t n = (count==_TRUNCATE || count>=dstcch) ? dstcch-1 : count;
    wcsncpy(dst, src, n);
    dst[n] = 0;
    return 0;
}
int wcsncat_s(wchar_t* dst, size_t dstcch, const wchar_t* src, size_t count){ wcsncat(dst, src, count); if(dstcch>0) dst[dstcch-1]=0; return 0; }
int swprintf_s(wchar_t* dst, size_t dstcch, const wchar_t* fmt, ...){ va_list ap; va_start(ap, fmt); int r = vswprintf(dst, dstcch, fmt, ap); va_end(ap); return (r<0)?1:0; }
int sprintf_s(char* dst, size_t dstcch, const char* fmt, ...){ va_list ap; va_start(ap, fmt); int r = vsnprintf(dst, dstcch, fmt, ap); va_end(ap); return (r<0)?1:0; }

BOOL PathIsUNCW(const wchar_t* path){ return FALSE; }
BOOL GetFileAttributesExW(const wchar_t* path, int infoLevel, WIN32_FILE_ATTRIBUTE_DATA* data){ return FALSE; }
BOOL GetVolumeInformationW(const wchar_t* a, wchar_t* b, DWORD c, DWORD* d, DWORD* e, DWORD* f, wchar_t* g, DWORD h){ return FALSE; }
BOOL GetDiskFreeSpaceExW(const wchar_t* a, ULARGE_INTEGER* b, ULARGE_INTEGER* c, ULARGE_INTEGER* d){ return FALSE; }
BOOL GetTempPathW(DWORD n, wchar_t* out){ if(out&&n>0) out[0]=0; return FALSE; }
BOOL GetTempFileNameW(const wchar_t* a, const wchar_t* b, DWORD c, wchar_t* out){ if(out) out[0]=0; return FALSE; }

static void wcs_to_u8(const wchar_t* w, char* u8, size_t n){
    wcstombs(u8, w, n);
    for(char* p=u8; *p; ++p){ if(*p=='\\') *p='/'; }
}

BOOL CreateDirectoryW(const wchar_t* path, void* sec){
    char u8[MAX_PATH*3]; wcs_to_u8(path,u8,sizeof(u8));
    if(mkdir(u8,0777)==0 || errno==EEXIST) return TRUE;
    return FALSE;
}

HANDLE CreateFileW(const wchar_t* path, DWORD a, DWORD b, void* c, DWORD d, DWORD e, HANDLE f){
    char u8[MAX_PATH*3]; wcs_to_u8(path,u8,sizeof(u8));
    int fd = open(u8, O_RDWR|O_CREAT, 0666);
    if(fd<0) return INVALID_HANDLE_VALUE;
    if(d==CREATE_ALWAYS) ftruncate(fd,0);
    return (HANDLE)(intptr_t)fd;
}

BOOL ReadFile(HANDLE h, void* buf, DWORD sz, DWORD* out, void* ov){
    int fd=(int)(intptr_t)h; ssize_t r=read(fd,buf,sz); if(r<0) return FALSE; if(out) *out=(DWORD)r; return TRUE;
}
BOOL WriteFile(HANDLE h, const void* buf, DWORD sz, DWORD* out, void* ov){
    int fd=(int)(intptr_t)h; ssize_t r=write(fd,buf,sz); if(r<0) return FALSE; if(out) *out=(DWORD)r; return TRUE;
}
BOOL CloseHandle(HANDLE h){ int fd=(int)(intptr_t)h; return close(fd)==0; }
DWORD SetFilePointer(HANDLE h, LONG off, void* high, DWORD method){ return (DWORD)lseek((int)(intptr_t)h, off, method); }
BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER off, LARGE_INTEGER* newpos, DWORD method){
    off_t r = lseek((int)(intptr_t)h, (off_t)off.QuadPart, method);
    if(r<0) return FALSE; if(newpos) newpos->QuadPart=r; return TRUE;
}
BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER* size){ struct stat st; if(fstat((int)(intptr_t)h,&st)!=0) return FALSE; if(size) size->QuadPart=st.st_size; return TRUE; }
DWORD GetLastError(void){ return 0; }
void GetSystemTimeAsFileTime(FILETIME* ft){ if(ft){ ft->dwLowDateTime=0; ft->dwHighDateTime=0; } }
BOOL FormatMessageA(DWORD flags, const void* src, DWORD err, DWORD lang, char* buf, DWORD size, void* args){ if(buf&&size>0){ snprintf(buf,size,"err %u",err); } return TRUE; }

int WideCharToMultiByte(UINT cp, DWORD flags, const wchar_t* w, int wn, char* u8, int u8n, const char* def, BOOL* used){
    size_t len = wcstombs(NULL,w,0);
    if(len==(size_t)-1) return 0;
    if(u8 && u8n>0){
        size_t r = wcstombs(u8,w,u8n);
        if(r==(size_t)-1) return 0;
        if(r<u8n) u8[r]=0;
        return (int)r;
    }
    return (int)(len+1);
}
int MultiByteToWideChar(UINT cp, DWORD flags, const char* u8, int u8n, wchar_t* w, int wn){
    size_t len = mbstowcs(NULL,u8,0);
    if(len==(size_t)-1) return 0;
    if(w && wn>0){
        size_t r = mbstowcs(w,u8,wn);
        if(r==(size_t)-1) return 0;
        if(r<wn) w[r]=0;
        return (int)r;
    }
    return (int)(len+1);
}

void* VirtualAlloc(void* addr, size_t size, DWORD mem, DWORD prot){ return malloc(size); }
BOOL VirtualFree(void* addr, size_t size, DWORD freeType){ free(addr); return TRUE; }
void* _aligned_malloc(size_t size, size_t alignment){ void* p=NULL; if(posix_memalign(&p, alignment, size)!=0) return NULL; return p; }
void _aligned_free(void* p){ free(p); }
