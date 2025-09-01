#include "windows.h"
#include <stdio.h>
#include <string.h>

BOOL GlobalMemoryStatusEx(MEMORYSTATUSEX* ms){ if(ms){ ms->ullAvailPhys = 0; } return 0; }
int wcscpy_s(wchar_t* dst, size_t dstcch, const wchar_t* src){ if(!dst||!src) return 1; wcsncpy(dst, src, dstcch); if(dstcch>0) dst[dstcch-1]=0; return 0; }
int wcsncat_s(wchar_t* dst, size_t dstcch, const wchar_t* src, size_t count){ wcsncat(dst, src, count); if(dstcch>0) dst[dstcch-1]=0; return 0; }
int swprintf_s(wchar_t* dst, size_t dstcch, const wchar_t* fmt, ...){ va_list ap; va_start(ap, fmt); int r = vswprintf(dst, dstcch, fmt, ap); va_end(ap); return (r<0)?1:0; }
int sprintf_s(char* dst, size_t dstcch, const char* fmt, ...){ va_list ap; va_start(ap, fmt); int r = vsnprintf(dst, dstcch, fmt, ap); va_end(ap); return (r<0)?1:0; }

BOOL PathIsUNCW(const wchar_t* path){ return FALSE; }
BOOL GetFileAttributesExW(const wchar_t* path, int infoLevel, WIN32_FILE_ATTRIBUTE_DATA* data){ return FALSE; }
BOOL GetVolumeInformationW(const wchar_t* a, wchar_t* b, DWORD c, DWORD* d, DWORD* e, DWORD* f, wchar_t* g, DWORD h){ return FALSE; }
BOOL GetDiskFreeSpaceExW(const wchar_t* a, ULARGE_INTEGER* b, ULARGE_INTEGER* c, ULARGE_INTEGER* d){ return FALSE; }
BOOL GetTempPathW(DWORD n, wchar_t* out){ if(out&&n>0) out[0]=0; return FALSE; }
BOOL GetTempFileNameW(const wchar_t* a, const wchar_t* b, DWORD c, wchar_t* out){ if(out) out[0]=0; return FALSE; }
HANDLE CreateFileW(const wchar_t* a, DWORD b, DWORD c, void* d, DWORD e, DWORD f, HANDLE g){ return INVALID_HANDLE_VALUE; }
BOOL ReadFile(HANDLE a, void* b, DWORD c, DWORD* d, void* e){ return FALSE; }
BOOL WriteFile(HANDLE a, const void* b, DWORD c, DWORD* d, void* e){ return FALSE; }
BOOL CloseHandle(HANDLE a){ return TRUE; }
DWORD SetFilePointer(HANDLE a, LONG b, void* c, DWORD d){ return 0; }
int WideCharToMultiByte(UINT cp, DWORD flags, const wchar_t* w, int wn, char* u8, int u8n, const char* def, BOOL* used){ if(u8 && w){ wcstombs(u8, w, u8n); if(u8n>0) u8[u8n-1]=0; } return 0; }
int MultiByteToWideChar(UINT cp, DWORD flags, const char* u8, int u8n, wchar_t* w, int wn){ if(w && u8){ mbstowcs(w, u8, wn); if(wn>0) w[wn-1]=0; } return 0; }
void* VirtualAlloc(void* addr, size_t size, DWORD mem, DWORD prot){ return malloc(size); }
BOOL VirtualFree(void* addr, size_t size, DWORD freeType){ free(addr); return TRUE; }
