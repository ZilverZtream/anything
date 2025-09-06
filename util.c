
#include "util.h"
#include <shlwapi.h>
#include <string.h>
#include <wchar.h>
#include <stdlib.h>
#include <malloc.h>
#include <immintrin.h>
#include <stdio.h>
#include <math.h>
#include <nmmintrin.h>
#include <bcrypt.h>
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "bcrypt.lib")

// default sort buffer size: 256MB
size_t g_sort_buffer_size = 256 * 1024 * 1024;

// helper to clamp work memory based on available physical memory
size_t dynamic_work_mem(void){
    MEMORYSTATUSEX ms; ms.dwLength = sizeof(ms);
    if(!GlobalMemoryStatusEx(&ms)) return g_sort_buffer_size;
    size_t avail = (size_t)(ms.ullAvailPhys / 4); // use 25% of free RAM
    if(avail > g_sort_buffer_size) return g_sort_buffer_size;
    if(avail < (1<<20)) return (1<<20);
    return avail;
}

void* aligned_malloc(size_t size, size_t alignment){
#ifdef _WIN32
    return _aligned_malloc(size, alignment);
#else
    void* p = NULL;
    if(posix_memalign(&p, alignment, size) != 0) return NULL;
    return p;
#endif
}

void aligned_free(void* p){
#ifdef _WIN32
    _aligned_free(p);
#else
    free(p);
#endif
}

static BOOL sb_reserve(SortBuffer* sb, size_t add){
    size_t need = sb->len + add;
    if(need > sb->cap){
        size_t newcap = sb->cap ? sb->cap * 2 : 256;
        while(newcap < need) newcap *= 2;
        uint8_t* p = (uint8_t*)realloc(sb->data, newcap);
        if(!p) return FALSE;
        sb->data = p; sb->cap = newcap;
    }
    return TRUE;
}

void sb_init(SortBuffer* sb){
    if(!sb) return; sb->data=NULL; sb->len=sb->cap=0;
}

void sb_free(SortBuffer* sb){
    if(!sb) return; if(sb->data) free(sb->data); sb->data=NULL; sb->len=sb->cap=0;
}

BOOL sb_pack_str(SortBuffer* sb, const char* s){
    size_t n = s ? strlen(s) : 0;
    if(!sb_reserve(sb, sizeof(uint32_t)+n)) return FALSE;
    uint32_t len=(uint32_t)n;
    memcpy(sb->data+sb->len, &len, sizeof(len));
    sb->len += sizeof(len);
    if(n){ memcpy(sb->data+sb->len, s, n); sb->len += n; }
    return TRUE;
}

BOOL sb_pack_u64(SortBuffer* sb, uint64_t v){
    if(!sb_reserve(sb, sizeof(v))) return FALSE;
    memcpy(sb->data+sb->len, &v, sizeof(v));
    sb->len += sizeof(v);
    return TRUE;
}
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
    int needed = WideCharToMultiByte(CP_UTF8,0,w,-1,NULL,0,NULL,NULL);
    if(needed <= 0){ u8[0] = 0; return; }
    if((size_t)needed <= u8cap){
        WideCharToMultiByte(CP_UTF8,0,w,-1,u8,(int)u8cap,NULL,NULL);
        u8[u8cap-1]=0;
    } else {
        char* tmp = (char*)malloc((size_t)needed);
        if(!tmp){ u8[0]=0; return; }
        WideCharToMultiByte(CP_UTF8,0,w,-1,tmp,needed,NULL,NULL);
        size_t copy = u8cap - 1;
        if((size_t)needed-1 < copy) copy = (size_t)needed-1;
        memcpy(u8, tmp, copy);
        u8[copy] = 0;
        free(tmp);
    }
}
void to_wide(const char* u8, wchar_t* w, size_t wcap){
    if(!u8 || !w || wcap==0){ if(w) w[0]=0; return; }
    int needed = MultiByteToWideChar(CP_UTF8,0,u8,-1,NULL,0);
    if(needed <= 0){ w[0] = 0; return; }
    if((size_t)needed <= wcap){
        MultiByteToWideChar(CP_UTF8,0,u8,-1,w,(int)wcap);
        w[wcap-1]=0;
    } else {
        wchar_t* tmp = (wchar_t*)malloc((size_t)needed * sizeof(wchar_t));
        if(!tmp){ w[0]=0; return; }
        MultiByteToWideChar(CP_UTF8,0,u8,-1,tmp,needed);
        size_t copy = wcap - 1;
        if((size_t)needed-1 < copy) copy = (size_t)needed-1;
        memcpy(w, tmp, copy * sizeof(wchar_t));
        w[copy] = 0;
        free(tmp);
    }
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

// extend an existing hash with more data
uint64_t hash64_add(uint64_t h, const void* data, size_t len){
    const uint8_t* p = (const uint8_t*)data;
    for(size_t i=0;i<len;i++){
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

uint64_t hash64_sort_key(const SortBuffer* sb){
    if(!sb || !sb->data) return 0;
    return hash64_add(1469598103934665603ULL, sb->data, sb->len);
}

uint64_t crc64_update(uint64_t crc, const void* data, size_t len){
    const uint8_t* p = (const uint8_t*)data;
    while(len >= 8){
        uint64_t chunk;
        memcpy(&chunk, p, sizeof(chunk));
        crc = _mm_crc32_u64(crc, chunk);
        p += 8; len -= 8;
    }
    while(len--){
        crc = _mm_crc32_u8((uint32_t)crc, *p++);
    }
    return crc;
}

uint64_t crc64(const void* data, size_t len){
    return crc64_update(0, data, len);
}

uint64_t crc64_file(const wchar_t* path){
    if(!path) return 0;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if(h==INVALID_HANDLE_VALUE) return 0;
    uint8_t buf[64*1024];
    DWORD rd = 0;
    uint64_t crc = 0;
    while(ReadFile(h, buf, sizeof(buf), &rd, NULL) && rd>0){
        crc = crc64_update(crc, buf, rd);
    }
    CloseHandle(h);
    return crc;
}

void sha1(const void* data, size_t len, uint8_t out[20]){
    if(!data || !out){ if(out) memset(out,0,20); return; }
    BCRYPT_ALG_HANDLE hAlg = NULL;
    DWORD hashObjSize=0, cbData=0;
    if(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA1_ALGORITHM, NULL, 0)!=0){ memset(out,0,20); return; }
    if(BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&hashObjSize, sizeof(hashObjSize), &cbData, 0)!=0){ BCryptCloseAlgorithmProvider(hAlg,0); memset(out,0,20); return; }
    PUCHAR hashObj = (PUCHAR)malloc(hashObjSize);
    if(!hashObj){ BCryptCloseAlgorithmProvider(hAlg,0); memset(out,0,20); return; }
    BCRYPT_HASH_HANDLE hHash = NULL;
    if(BCryptCreateHash(hAlg, &hHash, hashObj, hashObjSize, NULL, 0, 0)!=0){ free(hashObj); BCryptCloseAlgorithmProvider(hAlg,0); memset(out,0,20); return; }
    BCryptHashData(hHash, (PUCHAR)data, (ULONG)len, 0);
    BCryptFinishHash(hHash, out, 20, 0);
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg,0);
    free(hashObj);
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

void compute_drive_signature(const wchar_t* drive, uint8_t sig[32]){
    if(!drive || !sig){ return; }
    wchar_t vol[64];
    DWORD serial=0,max_comp=0,flags=0;
    if(!GetVolumeInformationW(drive, vol, 64, &serial, &max_comp, &flags, NULL, 0)){
        memset(sig,0,32);
        return;
    }
    ULARGE_INTEGER freeb,totalb;
    if(!GetDiskFreeSpaceExW(drive, &freeb, &totalb, NULL)){
        memset(sig,0,32);
        return;
    }
    char vol_utf8[128];
    to_utf8(vol, vol_utf8, sizeof(vol_utf8));
    char data[256];
    sprintf_s(data, sizeof(data), "%08X:%s:%llu", serial, vol_utf8, totalb.QuadPart);
    uint64_t h1 = hash64(data, strlen(data));
    uint64_t h2 = hash64(&h1, sizeof(h1));
    uint64_t h3 = hash64(&h2, sizeof(h2));
    uint64_t h4 = hash64(&h3, sizeof(h3));
    memcpy(sig, &h1, 8);
    memcpy(sig+8, &h2, 8);
    memcpy(sig+16, &h3, 8);
    memcpy(sig+24, &h4, 8);
}

float bm25_score(int tf, int doc_len, float avg_doc_len, int docs_total, int docs_with_term){
    const float k1 = 1.5f;
    const float b = 0.75f;
    if(tf<=0 || doc_len<=0 || avg_doc_len<=0.0f || docs_total<=0 || docs_with_term<=0){
        return 0.0f;
    }
    float idf = logf(( (float)docs_total - (float)docs_with_term + 0.5f) /
                     ((float)docs_with_term + 0.5f) + 1.0f);
    float denom = (float)tf + k1 * (1.0f - b + b * ((float)doc_len / avg_doc_len));
    return idf * ((float)tf * (k1 + 1.0f) / denom);
}

int levenshtein_distance(const char* a, size_t alen, const char* b, size_t blen){
    if(!a) return (int)blen;
    if(!b) return (int)alen;
    if(alen > 1024 || blen > 1024){
        return (int)(alen > blen ? alen : blen);
    }

    if(blen > alen){
        const char* tmp = a; a = b; b = tmp;
        size_t tmp_len = alen; alen = blen; blen = tmp_len;
    }

    size_t cols = blen + 1;
    size_t buf_size = cols * sizeof(int);
    BOOL heap = buf_size > 4096;
    int* col = heap ? (int*)malloc(buf_size) : (int*)_malloca(buf_size);
    if(!col){
        if(heap){ free(col); } else { /* _malloca failed, nothing to free */ }
        return (int)(alen>blen?alen:blen);
    }

    for(size_t j=0; j<cols; j++) col[j] = (int)j;
    for(size_t i=0; i<alen; i++){
        col[0] = (int)(i + 1);
        int last_diag = (int)i;
        for(size_t j=0; j<blen; j++){
            int old_diag = col[j + 1];
            int cost = (a[i] == b[j]) ? 0 : 1;
            int del = col[j + 1] + 1;
            int ins = col[j] + 1;
            int sub = last_diag + cost;
            int m = del < ins ? del : ins;
            if(sub < m) m = sub;
            col[j + 1] = m;
            last_diag = old_diag;
        }
    }

    int dist = col[blen];
    if(heap){ free(col); } else { _freea(col); }
    return dist;
}

BOOL fuzzy_match(const char* text, const char* pattern, int max_dist){
    if(!text || !pattern || max_dist < 0) return FALSE;
    size_t n = strlen(text), m = strlen(pattern);
    if(m == 0) return TRUE;
    if(n > 1024 || m > 1024 || max_dist > 1024) return FALSE; // Cap to prevent abuse
    if(n <= m){
        if((int)(m - n) > max_dist) return FALSE;
        return levenshtein_distance(text, n, pattern, m) <= max_dist;
    }
    for(size_t i = 0; i <= n - m; i++){
        size_t win = m + (size_t)max_dist;
        if(i + win > n) win = n - i;
        int d = levenshtein_distance(text + i, win, pattern, m);
        if(d <= max_dist) return TRUE;
        if(i > 0 && abs((int)(text[i] - pattern[0])) > max_dist) i += m / 2;
    }
    return FALSE;
}

void normalize_filename_utf8(const char* name_utf8, char* out, size_t outcap){
    if(!name_utf8 || !out || outcap==0){ if(out) out[0]=0; return; }

    /* If the caller supplied the same buffer for input and output we can
       operate directly on it without any additional allocations. */
    if(name_utf8 == out){
        size_t len=strlen(out), end=len;
        for(size_t i=len;i>0;i--){ if(out[i-1]=='.'){ end=i-1; break; } }
        size_t o=0;
        for(size_t i=0;i<end && o+1<outcap;i++){
            char c=out[i];
            if(c=='_'||c=='-'||c=='.') c=' ';
            if(c>='A'&&c<='Z') c=(char)(c-'A'+'a');
            out[o++]=c;
        }
        out[o]=0;
        return;
    }

    /* Allocate only as much memory as needed for the copy instead of using
       a fixed, over-sized buffer. */
    size_t len=strlen(name_utf8);
    char* tmp=(char*)malloc(len+1);
    if(!tmp){ out[0]=0; return; }
    memcpy(tmp,name_utf8,len+1);

    size_t end=len;
    for(size_t i=len;i>0;i--){ if(tmp[i-1]=='.'){ end=i-1; break; } }
    size_t o=0;
    for(size_t i=0;i<end && o+1<outcap;i++){
        char c=tmp[i];
        if(c=='_'||c=='-'||c=='.') c=' ';
        if(c>='A'&&c<='Z') c=(char)(c-'A'+'a');
        out[o++]=c;
    }
    out[o]=0;
    free(tmp);
}

typedef struct {
    HANDLE h;
    size_t remaining;
    uint8_t* item;
    BOOL loaded;
} SortChunk;

BOOL external_sort(const wchar_t* tmpdir, void* base, size_t n, size_t size,
                   int (*cmp)(const void*, const void*)){
    if(!base || !cmp || size==0) return FALSE;
    size_t max_items = g_sort_buffer_size / size;
    if(max_items==0) return FALSE;
    if(n <= max_items){ qsort(base, n, size, cmp); return TRUE; }

    wchar_t tmpbuf[MAX_PATH];
    if(!tmpdir){ GetTempPathW(MAX_PATH, tmpbuf); tmpdir = tmpbuf; }

    size_t offset=0, chunks_n=0;
    SortChunk* chunks=NULL;
    while(offset < n){
        size_t m = n - offset; if(m > max_items) m = max_items;
        qsort((uint8_t*)base + offset*size, m, size, cmp);
        wchar_t file[MAX_PATH];
        if(!GetTempFileNameW(tmpdir, L"srt", 0, file)) goto fail;
        HANDLE h = CreateFileW(file, GENERIC_READ|GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS,
                               FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,
                               NULL);
        if(h==INVALID_HANDLE_VALUE) goto fail;
        DWORD written=0;
        if(!WriteFile(h, (uint8_t*)base + offset*size, (DWORD)(m*size), &written, NULL) || written!=(DWORD)(m*size)){
            CloseHandle(h); goto fail;
        }
        SetFilePointer(h,0,NULL,FILE_BEGIN);
        SortChunk* tmp=(SortChunk*)realloc(chunks, (chunks_n+1)*sizeof(SortChunk));
        if(!tmp){ CloseHandle(h); goto fail; }
        chunks=tmp;
        chunks[chunks_n].h=h;
        chunks[chunks_n].remaining=m;
        chunks[chunks_n].item=(uint8_t*)VirtualAlloc(NULL, size, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
        chunks[chunks_n].loaded=FALSE;
        chunks_n++;
        offset += m;
    }

    for(size_t i=0;i<chunks_n;i++){
        DWORD rd=0;
        if(chunks[i].remaining>0 && ReadFile(chunks[i].h, chunks[i].item, (DWORD)size, &rd, NULL) && rd==size){
            chunks[i].remaining--; chunks[i].loaded=TRUE;
        }
    }

    size_t out_idx=0;
    while(TRUE){
        int best=-1;
        for(size_t i=0;i<chunks_n;i++){
            if(chunks[i].loaded){
                if(best==-1 || cmp(chunks[i].item, chunks[best].item)<0) best=(int)i;
            }
        }
        if(best==-1) break;
        memcpy((uint8_t*)base + out_idx*size, chunks[best].item, size);
        out_idx++;
        if(chunks[best].remaining>0){
            DWORD rd=0;
            if(ReadFile(chunks[best].h, chunks[best].item, (DWORD)size, &rd, NULL) && rd==size){
                chunks[best].remaining--; chunks[best].loaded=TRUE;
            } else {
                chunks[best].loaded=FALSE;
            }
        } else {
            chunks[best].loaded=FALSE;
        }
    }

    for(size_t i=0;i<chunks_n;i++){
        if(chunks[i].item) VirtualFree(chunks[i].item,0,MEM_RELEASE);
        CloseHandle(chunks[i].h);
    }
    free(chunks);
    return TRUE;
fail:
    if(chunks){
        for(size_t i=0;i<chunks_n;i++){
            if(chunks[i].item) VirtualFree(chunks[i].item,0,MEM_RELEASE);
            if(chunks[i].h!=INVALID_HANDLE_VALUE) CloseHandle(chunks[i].h);
        }
    }
    free(chunks);
    return FALSE;
}
