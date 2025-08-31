
#include "util.h"
#include <shlwapi.h>
#include <string.h>
#include <immintrin.h>
#pragma comment(lib, "shlwapi.lib")

void lowercase_ascii(char* s, size_t n){
    for(size_t i=0;i<n && s[i];++i){
        if(s[i]>='A' && s[i]<='Z') s[i] = (char)(s[i]-'A'+'a');
    }
}
void lowercase_wchar(wchar_t* s){
    for(size_t i=0;s[i];++i){
        if(s[i]>='A' && s[i]<='Z') s[i] = (wchar_t)(s[i]-L'A'+L'a');
    }
}
BOOL path_join(wchar_t* dst, size_t dstcch, const wchar_t* a, const wchar_t* b){
    if(!dst || !a || !b) return FALSE;
    if(wcscpy_s(dst, dstcch, a)!=0) return FALSE;
    size_t n = wcslen(dst);
    if(n>0 && dst[n-1]!=L'\\') { if(wcsncat_s(dst, dstcch, L"\\", 1)!=0) return FALSE; }
    return wcsncat_s(dst, dstcch, b, _TRUNCATE)==0;
}
BOOL path_dirname(const wchar_t* path, wchar_t* out, size_t outcch){
    if(!path || !out) return FALSE;
    if(wcscpy_s(out, outcch, path)!=0) return FALSE;
    wchar_t* p = wcsrchr(out, L'\\');
    if(!p) return FALSE;
    *p = 0;
    return TRUE;
}
BOOL get_drive_root(const wchar_t* any_path, wchar_t* root, size_t cch){
    if(!any_path || !root) return FALSE;
    if(PathIsUNCW(any_path)){
        const wchar_t* p = any_path;
        int slashes = 0;
        size_t i=0;
        while(*p && i+1<cch){
            root[i++] = *p;
            if(*p==L'\\') slashes++;
            if(slashes==4){ root[i]=0; return TRUE; }
            p++;
        }
        return FALSE;
    } else {
        if(wcslen(any_path)>=2 && any_path[1]==L':'){
            if(swprintf(root, cch, L"%c:\\", any_path[0])<=0) return FALSE;
            return TRUE;
        }
    }
    return FALSE;
}
uint64_t filetime_days(uint64_t ft){
    const uint64_t TICKS_PER_DAY = 864000000000ULL;
    return ft / TICKS_PER_DAY;
}
BOOL get_file_info_basic(const wchar_t* full, uint32_t* attrs, uint64_t* size, uint64_t* ctime, uint64_t* mtime, uint64_t* atime){
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if(!GetFileAttributesExW(full, GetFileExInfoStandard, &fad)) return FALSE;
    if(attrs) *attrs = fad.dwFileAttributes;
    if(size){ ULARGE_INTEGER s; s.LowPart=fad.nFileSizeLow; s.HighPart=fad.nFileSizeHigh; *size=s.QuadPart; }
    if(ctime){ ULARGE_INTEGER u; u.LowPart=fad.ftCreationTime.dwLowDateTime; u.HighPart=fad.ftCreationTime.dwHighDateTime; *ctime=u.QuadPart; }
    if(mtime){ ULARGE_INTEGER u; u.LowPart=fad.ftLastWriteTime.dwLowDateTime; u.HighPart=fad.ftLastWriteTime.dwHighDateTime; *mtime=u.QuadPart; }
    if(atime){ ULARGE_INTEGER u; u.LowPart=fad.ftLastAccessTime.dwLowDateTime; u.HighPart=fad.ftLastAccessTime.dwHighDateTime; *atime=u.QuadPart; }
    return TRUE;
}
void split_extension_utf8(const char* name_utf8, char* ext_out, size_t ext_len){
    ext_out[0]=0;
    const char* dot = strrchr(name_utf8, '.');
    if(dot && dot[1]){
        size_t n = strlen(dot+1);
        if(n >= ext_len) n = ext_len-1;
        memcpy(ext_out, dot+1, n);
        ext_out[n]=0;
        lowercase_ascii(ext_out, n);
    }
}
void to_utf8(const wchar_t* w, char* u8, size_t u8cap){
    if(!w || !u8 || u8cap==0){ if(u8) u8[0]=0; return; }
    WideCharToMultiByte(CP_UTF8,0,w,-1,u8,(int)u8cap,NULL,NULL);
    u8[u8cap-1]=0;
}
void to_wide(const char* u8, wchar_t* w, size_t wcap){
    if(!u8 || !w || wcap==0){ if(w) w[0]=0; return; }
    MultiByteToWideChar(CP_UTF8,0,u8,-1,w,(int)wcap);
    w[wcap-1]=0;
}
uint64_t hash64(const void* data, size_t len){
    const uint8_t* p = (const uint8_t*)data;
    uint64_t h = 1469598103934665603ULL;
    for(size_t i=0;i<len;i++){
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

// Runtime AVX2 check via CPUID
BOOL is_avx2_supported(void){
    int info[4]={0};
    __cpuid(info, 0);
    int max_id = info[0];
    if(max_id < 7) return FALSE;
    int info7[4]; __cpuidex(info7, 7, 0);
    return (info7[1] & (1<<5)) != 0; // AVX2 bit
}

// AVX2-accelerated substring search (ASCII, case-insensitive if caller lowercases both)
BOOL avx2_contains(const char* haystack, size_t hlen, const char* needle, size_t nlen){
    if(nlen==0) return TRUE;
    if(hlen<nlen) return FALSE;
    const unsigned char first = (unsigned char)needle[0];
    __m256i vf = _mm256_set1_epi8((char)first);
    size_t i=0;
    for(; i+32<=hlen; i+=32){
        __m256i chunk = _mm256_loadu_si256((const __m256i*)(haystack+i));
        __m256i eq = _mm256_cmpeq_epi8(chunk, vf);
        uint32_t mask = (uint32_t)_mm256_movemask_epi8(eq);
        while(mask){
            unsigned long bit=0; _BitScanForward(&bit, mask);
            size_t pos = i + (size_t)bit;
            if(pos + nlen <= hlen){
                if(memcmp(haystack+pos, needle, nlen)==0) return TRUE;
            }
            mask &= (mask-1);
        }
    }
    // tail
    for(; i+1<=hlen-nlen+1; i++){
        if(haystack[i]==(char)first){
            if(memcmp(haystack+i, needle, nlen)==0) return TRUE;
        }
    }
    return FALSE;
}
