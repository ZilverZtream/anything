
// anything.c — Orchestrator
#define _CRT_SECURE_NO_WARNINGS
#ifdef _WIN32
#include <windows.h>
#include <shlwapi.h>
#include <intrin.h>
#include <objbase.h>
#include <filter.h>
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "query.lib")
#else
#include <unistd.h>
#endif
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#ifdef __APPLE__
#include <CoreServices/CoreServices.h>
#include <CoreFoundation/CoreFoundation.h>
#endif

#include "anything.h"
#include "database.h"
#include "util.h"
#include "lmdb.h"
#include "archive.h"
#include "plugin.h"
#include "scanner.h"
#include "enterprise.h"
#include "config.h"

#include <stdbool.h>
#include <zip.h>
#include <libpst/libpst.h>
#ifndef _WIN32
#include <strings.h>
#include <stdarg.h>
#include <wctype.h>
#define Sleep(ms) usleep((ms)*1000)
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define _wcsicmp wcscasecmp
static int wcsncasecmp_local(const wchar_t* a, const wchar_t* b, size_t n){
    for(size_t i=0;i<n;i++){
        wchar_t ca = towlower(a[i]);
        wchar_t cb = towlower(b[i]);
        if(ca!=cb || ca==0 || cb==0) return ca - cb;
    }
    return 0;
}
static char* StrStrIA(const char* haystack, const char* needle){
    return strcasestr(haystack, needle);
}
static wchar_t* StrStrIW(const wchar_t* haystack, const wchar_t* needle){
    size_t nlen = wcslen(needle);
    for(const wchar_t* p=haystack; *p; p++){
        if(wcsncasecmp_local(p, needle, nlen) == 0) return (wchar_t*)p;
    }
    return NULL;
}
static int _snwprintf(wchar_t* dst, size_t cch, const wchar_t* fmt, ...){
    va_list ap; va_start(ap, fmt);
    int r = vswprintf(dst, cch, fmt, ap);
    va_end(ap);
    return r;
}
#define CP_UTF8 65001
#ifdef __APPLE__
static void utf8_to_wide(const char* src, wchar_t* dst, size_t dstlen){
    if(!dstlen) return;
    CFStringRef cf = CFStringCreateWithCString(NULL, src, kCFStringEncodingUTF8);
    if(!cf){ dst[0] = 0; return; }
    CFRange range = {0, CFStringGetLength(cf)};
#if __BIG_ENDIAN__
    CFStringEncoding enc = kCFStringEncodingUTF32BE;
#else
    CFStringEncoding enc = kCFStringEncodingUTF32LE;
#endif
    CFIndex used = 0;
    CFStringGetBytes(cf, range, enc, 0, false, (UInt8*)dst,
                     (dstlen - 1) * sizeof(wchar_t), &used);
    CFRelease(cf);
    size_t chars = (size_t)used / sizeof(wchar_t);
    dst[chars] = 0;
}
static void wide_to_utf8(const wchar_t* src, char* dst, size_t dstlen){
    if(!dstlen) return;
#if __BIG_ENDIAN__
    CFStringEncoding enc = kCFStringEncodingUTF32BE;
#else
    CFStringEncoding enc = kCFStringEncodingUTF32LE;
#endif
    size_t wlen = wcslen(src);
    CFStringRef cf = CFStringCreateWithBytes(NULL, (const UInt8*)src,
                                             wlen * sizeof(wchar_t), enc, false);
    if(!cf){ dst[0] = 0; return; }
    CFStringGetCString(cf, dst, dstlen, kCFStringEncodingUTF8);
    CFRelease(cf);
}
static int MultiByteToWideChar(unsigned int cp, unsigned int flags, const char* src, int srclen, wchar_t* dst, int dstlen){
    (void)cp; (void)flags; (void)srclen;
    if(!dst) {
        CFStringRef cf = CFStringCreateWithCString(NULL, src, kCFStringEncodingUTF8);
        if(!cf) return 0;
        int len = (int)CFStringGetLength(cf) + 1;
        CFRelease(cf);
        return len;
    }
    utf8_to_wide(src, dst, dstlen);
    return (int)wcslen(dst);
}
static int WideCharToMultiByte(unsigned int cp, unsigned int flags, const wchar_t* src, int srclen, char* dst, int dstlen, void* a, void* b){
    (void)cp; (void)flags; (void)srclen; (void)a; (void)b;
    wide_to_utf8(src, dst, dstlen);
    return (int)strlen(dst);
}
#else
static int MultiByteToWideChar(unsigned int cp, unsigned int flags, const char* src, int srclen, wchar_t* dst, int dstlen){
    (void)cp; (void)flags; (void)srclen;
    return mbstowcs(dst, src, dstlen);
}
static int WideCharToMultiByte(unsigned int cp, unsigned int flags, const wchar_t* src, int srclen, char* dst, int dstlen, void* a, void* b){
    (void)cp; (void)flags; (void)srclen; (void)a; (void)b;
    return wcstombs(dst, src, dstlen);
}
#endif
#endif

static MPMCQueue g_live_updates;
static BOOL g_live_inited = FALSE;

void live_updates_init(void){
    if(!g_live_inited){
        MPMC_Init(&g_live_updates, 1<<12);
        g_live_inited = TRUE;
    }
}

BOOL live_updates_poll(LiveUpdate* out){
    if(!g_live_inited) return FALSE;
    void* p = NULL;
    if(!MPMC_Pop(&g_live_updates, &p)) return FALSE;
    if(!p) return FALSE;
    LiveUpdate* lu = (LiveUpdate*)p;
    *out = *lu;
    aligned_free(lu);
    return TRUE;
}

static void push_live_update(const DbWorkItem* wi){
    if(!g_live_inited) return;
    LiveUpdate* lu = (LiveUpdate*)aligned_malloc(sizeof(LiveUpdate), CACHE_LINE_SIZE);
    if(!lu) return;
    wcscpy_s(lu->parent_path, MAX_LONG_PATH, wi->parent_path);
    wcscpy_s(lu->name, MAX_PATH, wi->name);
    lu->op = wi->op;
    while(!MPMC_Push(&g_live_updates, lu)){
        if(!g_live_inited){
            aligned_free(lu);
            return;
        }
        Sleep(0);
    }
}

#include "metadata.h"

// ---- MPMC queue implementation ----
typedef enum { CONTENT_NONE, CONTENT_TEXT, CONTENT_IFILTER, CONTENT_EMAIL, CONTENT_EPUB, CONTENT_PST } ContentMode;

static ContentMode get_content_mode(const wchar_t* name){
    const wchar_t* ext = wcsrchr(name, L'.');
    if(!ext) return CONTENT_NONE;
    ext++;
    if(_wcsicmp(ext,L"txt")==0 || _wcsicmp(ext,L"md")==0 ||
       _wcsicmp(ext,L"c")==0   || _wcsicmp(ext,L"h")==0   ||
       _wcsicmp(ext,L"cpp")==0 || _wcsicmp(ext,L"hpp")==0 ||
       _wcsicmp(ext,L"py")==0  || _wcsicmp(ext,L"js")==0  ||
       _wcsicmp(ext,L"cs")==0  || _wcsicmp(ext,L"vb")==0  ||
       _wcsicmp(ext,L"r")==0   || _wcsicmp(ext,L"java")==0 ||
       _wcsicmp(ext,L"json")==0 || _wcsicmp(ext,L"xml")==0 ||
       _wcsicmp(ext,L"yaml")==0 || _wcsicmp(ext,L"yml")==0 ||
       _wcsicmp(ext,L"toml")==0 || _wcsicmp(ext,L"csv")==0 ||
       _wcsicmp(ext,L"nfo")==0  || _wcsicmp(ext,L"ini")==0 ||
       _wcsicmp(ext,L"log")==0  || _wcsicmp(ext,L"rtf")==0 ||
       _wcsicmp(ext,L"sql")==0)
        return CONTENT_TEXT;

    if(_wcsicmp(ext,L"pdf")==0  || _wcsicmp(ext,L"doc")==0  ||
       _wcsicmp(ext,L"docx")==0 || _wcsicmp(ext,L"ppt")==0  ||
       _wcsicmp(ext,L"pptx")==0 || _wcsicmp(ext,L"xls")==0  ||
       _wcsicmp(ext,L"xlsx")==0)
        return CONTENT_IFILTER;

    if(_wcsicmp(ext,L"eml")==0 || _wcsicmp(ext,L"emlx")==0)
        return CONTENT_EMAIL;

    if(_wcsicmp(ext,L"epub")==0)
        return CONTENT_EPUB;

    if(_wcsicmp(ext,L"pst")==0)
        return CONTENT_PST;

    return CONTENT_NONE;
}

static BOOL needs_thumbnail(const wchar_t* name){
    const wchar_t* ext = wcsrchr(name, L'.');
    if(!ext) return FALSE;
    ext++;
    const wchar_t* exts[] = {L"pdf", L"doc", L"docx", L"ppt", L"pptx", L"xls", L"xlsx"};
    for(size_t i=0;i<sizeof(exts)/sizeof(exts[0]);i++){
        if(_wcsicmp(ext, exts[i])==0) return TRUE;
    }
    return FALSE;
}

static BOOL is_archive_file(const wchar_t* name){
    const wchar_t* ext = wcsrchr(name, L'.');
    if(!ext) return FALSE;
    ext++;
    static const wchar_t* exts[] = {
        L"zip", L"rar", L"7z", L"tar", L"cpio", L"ar", L"iso", L"cab", L"xar", L"lha", L"lzh",
        L"gz", L"bz2", L"xz", L"z", L"lzma", L"lz4", L"zst", L"tgz", L"tbz", L"tbz2", L"txz",
        L"tlz", L"tzst"
    };
    for(size_t i=0;i<sizeof(exts)/sizeof(exts[0]);i++){
        if(_wcsicmp(ext, exts[i])==0) return TRUE;
    }
    return FALSE;
}

#define MAX_INDEXED_CONTENT (1024*1024) // 1MB

static wchar_t* extract_with_filter(const wchar_t* path){
#ifdef _WIN32
    if(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)!=S_OK) return NULL;
    IFilter* filter=NULL;
    HRESULT hr = LoadIFilter(path, NULL, NULL, 0, 0, 0, &filter);
    if(FAILED(hr)) { CoUninitialize(); return NULL; }
    hr = filter->Init(IFILTER_INIT_APPLY_INDEX_ATTRIBUTES, 0, NULL, NULL);
    if(FAILED(hr)) { filter->Release(); CoUninitialize(); return NULL; }
    size_t cap=4096, len=0;
    wchar_t* out = (wchar_t*)malloc(cap*sizeof(wchar_t));
    if(!out){ filter->Release(); CoUninitialize(); return NULL; }
    for(;;){
        WCHAR buf[1024]; ULONG cch=1024;
        hr = filter->GetText(&cch, buf);
        if(hr!=S_OK || cch==0) break;
        if(len + cch + 1 > cap){
            cap = (cap + cch + 1)*2;
            wchar_t* tmp = (wchar_t*)realloc(out, cap*sizeof(wchar_t));
            if(!tmp){ free(out); out=NULL; break; }
            out = tmp;
        }
        memcpy(out+len, buf, cch*sizeof(wchar_t));
        len += cch;
    }
    if(out){
        out[len]=0;
    }
    filter->Release();
    CoUninitialize();
    return out;
#elif defined(__APPLE__)
    char utf8[MAX_LONG_PATH];
    wcstombs(utf8, path, sizeof(utf8));
    CFStringRef cfpath = CFStringCreateWithCString(NULL, utf8, kCFStringEncodingUTF8);
    if(!cfpath) return NULL;
    CFURLRef url = CFURLCreateWithFileSystemPath(NULL, cfpath, kCFURLPOSIXPathStyle, false);
    CFRelease(cfpath);
    if(!url) return NULL;
    MDItemRef item = MDItemCreate(NULL, url);
    CFRelease(url);
    if(!item) return NULL;
    CFStringRef text = MDItemCopyAttribute(item, kMDItemTextContent);
    CFRelease(item);
    if(!text) return NULL;
    CFIndex len = CFStringGetLength(text);
    CFIndex max = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
    char* buf = (char*)malloc((size_t)max);
    if(!buf){ CFRelease(text); return NULL; }
    if(!CFStringGetCString(text, buf, max, kCFStringEncodingUTF8)){
        free(buf);
        CFRelease(text);
        return NULL;
    }
    CFRelease(text);
    int wlen = MultiByteToWideChar(CP_UTF8, 0, buf, -1, NULL, 0);
    wchar_t* wout = (wchar_t*)malloc(sizeof(wchar_t)*wlen);
    if(wout) MultiByteToWideChar(CP_UTF8, 0, buf, -1, wout, wlen);
    free(buf);
    return wout;
#else
    (void)path;
    return NULL;
#endif
}

static wchar_t* extract_email_content(Db* db, const wchar_t* path, uint64_t* author_out, uint64_t* title_out){
    *author_out = 0;
    *title_out = 0;
    FILE* f = _wfopen(path, L"rb");
    if(!f) return NULL;
    if(fseek(f,0,SEEK_END)!=0){ fclose(f); return NULL; }
    long size = ftell(f);
    if(size < 0){ fclose(f); return NULL; }
    if(size > MAX_INDEXED_CONTENT) size = MAX_INDEXED_CONTENT;
    rewind(f);
    char* buf = (char*)malloc((size_t)size + 1);
    if(!buf){ fclose(f); return NULL; }
    size_t n = fread(buf,1,(size_t)size,f);
    fclose(f);
    buf[n]=0;
    char* header_end = strstr(buf, "\r\n\r\n");
    int advance = 4;
    if(!header_end){ header_end = strstr(buf, "\n\n"); advance = 2; }
    if(header_end){
        char* line = buf;
        while(line < header_end){
            char* next = strstr(line, "\r\n");
            size_t sep = 2;
            if(!next || next > header_end){
                next = strstr(line, "\n");
                sep = 1;
            }
            if(!next || next > header_end) break;
            size_t len = (size_t)(next - line);
            if(_strnicmp(line, "From:",5)==0){
                char tmp[256]; if(len>255) len=255; memcpy(tmp,line+5,len-5); tmp[len-5]=0;
                wchar_t wtmp[256]; to_wide(tmp,wtmp,256); *author_out = db_intern_wstring(db,wtmp);
            } else if(_strnicmp(line, "Subject:",8)==0){
                char tmp[256]; if(len>255) len=255; memcpy(tmp,line+8,len-8); tmp[len-8]=0;
                wchar_t wtmp[256]; to_wide(tmp,wtmp,256); *title_out = db_intern_wstring(db,wtmp);
            }
            line = next + sep;
        }
        header_end += advance;
    } else {
        header_end = buf;
    }
    char* body = header_end;
    int wlen = MultiByteToWideChar(CP_UTF8,0,body,-1,NULL,0);
    wchar_t* wbuf = (wchar_t*)malloc(sizeof(wchar_t)*wlen);
    if(wbuf) MultiByteToWideChar(CP_UTF8,0,body,-1,wbuf,wlen);
    free(buf);
    return wbuf;
}

static char* strip_html_tags(const char* in){
    size_t len = strlen(in);
    char* out = (char*)malloc(len+1);
    if(!out) return NULL;
    size_t o=0; int tag=0;
    for(size_t i=0;i<len;i++){
        if(in[i]=='<') tag=1;
        else if(in[i]=='>') tag=0;
        else if(!tag) out[o++]=in[i];
    }
    out[o]=0;
    return out;
}

static wchar_t* extract_epub_content(Db* db, const wchar_t* path, uint64_t* author_out, uint64_t* title_out){
    *author_out = 0;
    *title_out = 0;
    char u8[MAX_LONG_PATH*3];
    to_utf8(path, u8, sizeof(u8));
    int err=0; zip_t* z = zip_open(u8,0,&err);
    if(!z) return NULL;
    zip_stat_t st; char root[256]={0};
    if(zip_stat(z,"META-INF/container.xml",0,&st)==0){
        char* cbuf=(char*)malloc(st.size+1);
        if(cbuf){
            zip_file_t* cf=zip_fopen(z,"META-INF/container.xml",0);
            zip_fread(cf,cbuf,st.size); zip_fclose(cf); cbuf[st.size]=0;
            char* fp=strstr(cbuf,"full-path=");
            if(fp){ fp=strchr(fp,'"'); if(fp){ fp++; char* end=strchr(fp,'"'); if(end){ size_t l=end-fp; if(l>255) l=255; memcpy(root,fp,l); root[l]=0; }}}
            free(cbuf);
        }
    }
    if(root[0]){
        if(zip_stat(z,root,0,&st)==0){
            char* obuf=(char*)malloc(st.size+1);
            if(obuf){
                zip_file_t* of=zip_fopen(z,root,0);
                zip_fread(of,obuf,st.size); zip_fclose(of); obuf[st.size]=0;
                char* t=strstr(obuf,"<dc:title");
                if(t){ t=strchr(t,'>'); if(t){ t++; char* e=strstr(t,"</dc:title>"); if(e){ char tmp[256]; size_t l=e-t; if(l>255) l=255; memcpy(tmp,t,l); tmp[l]=0; wchar_t wtmp[256]; to_wide(tmp,wtmp,256); *title_out=db_intern_wstring(db,wtmp); }}}
                char* a=strstr(obuf,"<dc:creator");
                if(a){ a=strchr(a,'>'); if(a){ a++; char* e=strstr(a,"</dc:creator>"); if(e){ char tmp[256]; size_t l=e-a; if(l>255) l=255; memcpy(tmp,a,l); tmp[l]=0; wchar_t wtmp[256]; to_wide(tmp,wtmp,256); *author_out=db_intern_wstring(db,wtmp); }}}
                free(obuf);
            }
        }
    }
    size_t cap=4096,len=0; char* textbuf=(char*)malloc(cap);
    if(textbuf) textbuf[0]=0;
    zip_int64_t count=zip_get_num_entries(z,0);
    for(zip_int64_t i=0;i<count;i++){
        const char* name=zip_get_name(z,i,0);
        if(!name) continue;
        size_t namelen=strlen(name);
        if(namelen>5 && (_stricmp(name+namelen-5,".html")==0 || _stricmp(name+namelen-6,".xhtml")==0)){
            if(zip_stat_index(z,i,0,&st)!=0) continue;
            if(len + st.size >= MAX_INDEXED_CONTENT) break;
            char* buf=(char*)malloc(st.size+1);
            if(!buf) continue;
            zip_file_t* f=zip_fopen_index(z,i,0);
            zip_fread(f,buf,st.size); zip_fclose(f); buf[st.size]=0;
            char* stripped=strip_html_tags(buf); free(buf);
            if(!stripped) continue;
            size_t slen=strlen(stripped);
            if(len + slen +1 > cap){
                cap=(cap+slen+1)*2; char* tmp=(char*)realloc(textbuf,cap); if(!tmp){ free(stripped); break; } textbuf=tmp;
            }
            memcpy(textbuf+len,stripped,slen); len+=slen; textbuf[len]=0; free(stripped);
        }
    }
    zip_close(z);
    wchar_t* wbuf=NULL;
    if(textbuf){
        int wlen=MultiByteToWideChar(CP_UTF8,0,textbuf,-1,NULL,0);
        wbuf=(wchar_t*)malloc(sizeof(wchar_t)*wlen);
        if(wbuf) MultiByteToWideChar(CP_UTF8,0,textbuf,-1,wbuf,wlen);
        free(textbuf);
    }
    return wbuf;
}

static void append_utf8_line(char** buf, size_t* len, size_t* cap, const char* src){
    if(!src) return;
    size_t slen = strlen(src);
    if(*len + slen + 2 > MAX_INDEXED_CONTENT){
        if(*len >= MAX_INDEXED_CONTENT) return;
        slen = MAX_INDEXED_CONTENT - *len - 1;
    }
    if(*len + slen + 2 > *cap){
        size_t newcap = (*cap + slen + 2)*2;
        char* tmp = (char*)realloc(*buf, newcap);
        if(!tmp) return;
        *buf = tmp; *cap = newcap;
    }
    memcpy(*buf + *len, src, slen);
    *len += slen;
    (*buf)[(*len)++]='\n';
    (*buf)[*len]=0;
}

static void walk_pst_tree(pst_file* pf, pst_desc_tree* node, char** buf, size_t* len, size_t* cap){
    for(pst_desc_tree* cur=node; cur; cur=cur->next){
        pst_item* item = pst_parse_item(pf, cur, NULL);
        if(item){
            pst_convert_utf8_null(item, &item->subject);
            pst_convert_utf8_null(item, &item->body);
            if(item->email){
                pst_convert_utf8_null(item, &item->email->sender_address);
                if(item->email->sender_address.str)
                    append_utf8_line(buf,len,cap,item->email->sender_address.str);
            }
            if(item->subject.str) append_utf8_line(buf,len,cap,item->subject.str);
            if(item->body.str) append_utf8_line(buf,len,cap,item->body.str);
            pst_freeItem(item);
        }
        if(cur->child) walk_pst_tree(pf, cur->child, buf, len, cap);
    }
}

static wchar_t* extract_pst_content(Db* db, const wchar_t* path, uint64_t* author_out, uint64_t* title_out){
    *author_out = 0;
    *title_out = 0;
    (void)db;
    char u8[MAX_LONG_PATH*3];
    to_utf8(path, u8, sizeof(u8));
    pst_file pf; memset(&pf,0,sizeof(pf));
    if(pst_open(&pf, u8, NULL)!=0) return NULL;
    if(pst_load_index(&pf)!=0){ pst_close(&pf); return NULL; }
    if(pst_load_extended_attributes(&pf)!=0){ pst_close(&pf); return NULL; }
    pst_item* root = pst_parse_item(&pf, pf.d_head, NULL);
    if(!root){ pst_close(&pf); return NULL; }
    pst_desc_tree* top = pst_getTopOfFolders(&pf, root);
    size_t cap=4096,len=0; char* buf=(char*)malloc(cap);
    if(buf) buf[0]=0;
    if(top && top->child) walk_pst_tree(&pf, top->child, &buf, &len, &cap);
    pst_freeItem(root);
    pst_close(&pf);
    wchar_t* wbuf=NULL;
    if(buf){
        int wlen=MultiByteToWideChar(CP_UTF8,0,buf,-1,NULL,0);
        wbuf=(wchar_t*)malloc(sizeof(wchar_t)*wlen);
        if(wbuf) MultiByteToWideChar(CP_UTF8,0,buf,-1,wbuf,wlen);
        free(buf);
    }
    return wbuf;
}

static uint64_t index_file_content(Db* db, const wchar_t* parent, const wchar_t* name, uint64_t* author_out, uint64_t* title_out){
    *author_out = 0;
    *title_out = 0;
    ContentMode mode = get_content_mode(name);
    if(mode==CONTENT_NONE) return 0;
    wchar_t path[MAX_LONG_PATH];
    _snwprintf(path, MAX_LONG_PATH, L"%s\\%s", parent, name);

    wchar_t* wbuf = NULL;
    if(mode == CONTENT_TEXT){
        FILE* f = _wfopen(path, L"rb");
        if(!f) return 0;
        if(fseek(f,0,SEEK_END)!=0){ fclose(f); return 0; }
        long size = ftell(f);
        if(size < 0){ fclose(f); return 0; }
        if(size > MAX_INDEXED_CONTENT) size = MAX_INDEXED_CONTENT;
        rewind(f);
        char* buf = (char*)malloc((size_t)size + 1);
        if(!buf){ fclose(f); return 0; }
        size_t n = fread(buf,1,(size_t)size,f);
        fclose(f);
        buf[n]=0;
        for(size_t i=0;i<n;i++){ if(buf[i]==0){ buf[0]=0; break; } }
        if(buf[0]==0){ free(buf); return 0; }
        char* a = StrStrIA(buf, "author:");
        if(a){
            a += 7;
            while(*a==' '||*a=='\t') a++;
            char tmp[256]; size_t len=0;
            while(a[len] && a[len]!='\r' && a[len]!='\n' && len<255) len++;
            memcpy(tmp,a,len); tmp[len]=0;
            wchar_t wa[256]; to_wide(tmp, wa, 256);
            *author_out = db_intern_wstring(db, wa);
        }
        char* t = StrStrIA(buf, "title:");
        if(t){
            t += 6;
            while(*t==' '||*t=='\t') t++;
            char tmp[256]; size_t len=0;
            while(t[len] && t[len]!='\r' && t[len]!='\n' && len<255) len++;
            memcpy(tmp,t,len); tmp[len]=0;
            wchar_t wt[256]; to_wide(tmp, wt, 256);
            *title_out = db_intern_wstring(db, wt);
        }
        int wlen = MultiByteToWideChar(CP_UTF8,0,buf,-1,NULL,0);
        wbuf = (wchar_t*)malloc(sizeof(wchar_t)*wlen);
        if(wbuf) MultiByteToWideChar(CP_UTF8,0,buf,-1,wbuf,wlen);
        free(buf);
    } else if(mode == CONTENT_IFILTER){
        wbuf = extract_with_filter(path);
    } else if(mode == CONTENT_EMAIL){
        wbuf = extract_email_content(db, path, author_out, title_out);
    } else if(mode == CONTENT_EPUB){
        wbuf = extract_epub_content(db, path, author_out, title_out);
    } else if(mode == CONTENT_PST){
        wbuf = extract_pst_content(db, path, author_out, title_out);
    }
    if(!wbuf) return 0;
    wchar_t* meta_a = StrStrIW(wbuf, L"author:");
    if(meta_a){
        meta_a += 7;
        while(*meta_a==L' '||*meta_a==L'\t') meta_a++;
        wchar_t tmp[256]; size_t len=0;
        while(meta_a[len] && meta_a[len]!=L'\r' && meta_a[len]!=L'\n' && len<255) len++;
        wcsncpy_s(tmp,256,meta_a,len);
        *author_out = db_intern_wstring(db, tmp);
    }
    wchar_t* meta_t = StrStrIW(wbuf, L"title:");
    if(meta_t){
        meta_t += 6;
        while(*meta_t==L' '||*meta_t==L'\t') meta_t++;
        wchar_t tmp[256]; size_t len=0;
        while(meta_t[len] && meta_t[len]!=L'\r' && meta_t[len]!=L'\n' && len<255) len++;
        wcsncpy_s(tmp,256,meta_t,len);
        *title_out = db_intern_wstring(db, tmp);
    }
    uint64_t id = db_intern_wstring(db, wbuf);
    free(wbuf);
    return id;
}

BOOL MPMC_Init(MPMCQueue* q, LONG pow2_size){
    if(!q) return FALSE;
    LONG size=1; while(size<pow2_size) size<<=1;
    q->mask = size-1;
    size_t cells_size = sizeof(MPMCCell) * (size_t)size;
    q->cells = (MPMCCell*)aligned_malloc(cells_size, CACHE_LINE_SIZE);
    if(!q->cells) return FALSE;
    ZeroMemory(q->cells, cells_size);
    for(LONG i=0;i<size;i++){ q->cells[i].seq = i; }
    q->head=0; q->tail=0;
    return TRUE;
}
void MPMC_Destroy(MPMCQueue* q){
    if(!q || !q->cells) return;
    aligned_free(q->cells); q->cells=NULL;
}
BOOL MPMC_Push(MPMCQueue* q, void* data){
    MPMCCell* cell; LONG64 pos = q->head;
    for(;;){
        cell = &q->cells[pos & q->mask];
        LONG64 seq = cell->seq;
        if(seq == pos){
            if(InterlockedCompareExchange64(&q->head, pos+1, pos)==pos){
                cell->data = data;
                _ReadWriteBarrier();
                cell->seq = pos+1;
                return TRUE;
            }
        } else if(seq < pos) {
            return FALSE; // full
        } else {
            pos = q->head;
        }
        SwitchToThread();
    }
}
BOOL MPMC_Pop(MPMCQueue* q, void** out){
    MPMCCell* cell; LONG64 pos = q->tail;
    for(;;){
        cell = &q->cells[pos & q->mask];
        LONG64 seq = cell->seq;
        if(seq == pos+1){
            if(InterlockedCompareExchange64(&q->tail, pos+1, pos)==pos){
                void* d = cell->data;
                _ReadWriteBarrier();
                cell->seq = pos + q->mask + 1;
                *out = d;
                return TRUE;
            }
        } else if(seq < pos+1){
            return FALSE; // empty
        } else {
            pos = q->tail;
        }
        SwitchToThread();
    }
}

// ---- Writer context & thread ----
typedef struct WriterCtx {
    Db* db;
    int batch_size;
    volatile BOOL done;
    MPMCQueue queue;
    CancelToken cancel;
    size_t grow_attempts;
} WriterCtx;

static const size_t MAP_GROWTH_INCREMENT = 1ull * 1024ull * 1024ull * 1024ull; // 1 GB
static BOOL put_batch_with_growth(WriterCtx* ctx, DbRecord* buf, size_t in_batch){
    for(;;){
        if(db_put_records(ctx->db, buf, in_batch)){
            return TRUE;
        }
        const DbError* err = db_last_error(ctx->db);
        if(err->code == DB_ERROR_LMDB && err->detail == MDB_MAP_FULL){
            db_abort_write(ctx->db);
            size_t cur = db_current_mapsize(ctx->db);
            size_t max = db_max_mapsize(ctx->db);
            if(cur >= max){
                fprintf(stderr, "DB mapsize at max; cannot grow beyond %llu bytes\n", (unsigned long long)max);
                return FALSE;
            }
            size_t newsize = cur + MAP_GROWTH_INCREMENT;
            if(newsize > max) newsize = max;
            if(!db_set_mapsize(ctx->db, newsize)){
                const DbError* serr = db_last_error(ctx->db);
                fprintf(stderr, "db_set_mapsize failed: %s (code=%d)\n", serr->message, serr->detail);
                return FALSE;
            }
            if(!db_begin_write(ctx->db)) return FALSE;
            ctx->grow_attempts++;
            continue; // retry put
        } else {
            fprintf(stderr, "db_put_records failed: %s (code=%d)\n", err->message, err->detail);
            return FALSE;
        }
    }
}

static DWORD WINAPI DbWriterThread(void* p){
    WriterCtx* ctx = (WriterCtx*)p;
    if(!db_begin_write(ctx->db)) return 1;
    size_t in_batch = 0;
    DbRecord* buf = (DbRecord*)malloc(sizeof(DbRecord) * ctx->batch_size);
    if(!buf) return 1;
    ZeroMemory(buf, sizeof(DbRecord)*ctx->batch_size);

    for(;;){
        void* item = NULL;
        if(!MPMC_Pop(&ctx->queue, &item)){
            if(ctx->done) break;
            Sleep(1);
            continue;
        }
        if(item == NULL) break; // sentinel
        DbWorkItem* wi = (DbWorkItem*)item;
        if(wi->stage == INDEX_NAMES_ONLY || wi->op == WI_DELETE) push_live_update(wi);
        if(wi->op == WI_DELETE){
            db_delete_path(ctx->db, wi->parent_path, wi->name);
            aligned_free(wi);
            continue;
        }
        if(wi->stage == INDEX_NAMES_ONLY){
            DbRecord r = {0};
            r.type = (wi->attributes & FILE_ATTRIBUTE_DIRECTORY) ? DB_REC_DIR : DB_REC_FILE;
            r.parent_str_id = db_intern_wstring(ctx->db, wi->parent_path);
            r.name_str_id   = db_intern_wstring(ctx->db, wi->name);
            r.attributes    = wi->attributes;
            buf[in_batch++] = r;
            DbWorkItem* next = (DbWorkItem*)aligned_malloc(sizeof(DbWorkItem), CACHE_LINE_SIZE);
            if(next){
                next->content = NULL; next->preview = NULL;
                wcscpy_s(next->parent_path, MAX_LONG_PATH, wi->parent_path);
                wcscpy_s(next->name, MAX_PATH, wi->name);
                next->file_size = next->creation_time = next->modified_time = next->access_time = 0;
                next->attributes = wi->attributes;
                next->clone_id = 0;
                next->stage = INDEX_METADATA_LIGHT; next->op = WI_ADD;
                while(!MPMC_Push(&ctx->queue, next)) Sleep(0);
            }
            aligned_free(wi);
        } else if(wi->stage == INDEX_METADATA_LIGHT){
            DbRecord r = {0};
            r.type = (wi->attributes & FILE_ATTRIBUTE_DIRECTORY) ? DB_REC_DIR : DB_REC_FILE;
            r.parent_str_id = db_intern_wstring(ctx->db, wi->parent_path);
            r.name_str_id   = db_intern_wstring(ctx->db, wi->name);
            wchar_t full[MAX_LONG_PATH];
            _snwprintf(full, MAX_LONG_PATH, L"%s\\%s", wi->parent_path, wi->name);
            uint32_t attrs=0; uint64_t sz=0, ct=0, mt=0, at=0;
            get_file_info_basic(full, &attrs, &sz, &ct, &mt, &at);
            r.attributes = attrs;
            r.file_size = sz;
            r.creation_time = ct;
            r.modified_time = mt;
            r.access_time   = at;
            buf[in_batch++] = r;
            if(!(attrs & FILE_ATTRIBUTE_DIRECTORY)){
                DbWorkItem* next = (DbWorkItem*)aligned_malloc(sizeof(DbWorkItem), CACHE_LINE_SIZE);
                if(next){
                    next->content = NULL; next->preview = NULL;
                    wcscpy_s(next->parent_path, MAX_LONG_PATH, wi->parent_path);
                    wcscpy_s(next->name, MAX_PATH, wi->name);
                    next->file_size = sz; next->creation_time=ct; next->modified_time=mt; next->access_time=at;
                    next->attributes = attrs;
                    next->clone_id = 0;
                    next->stage = INDEX_FULL_CONTENT; next->op = WI_ADD;
                    while(!MPMC_Push(&ctx->queue, next)) Sleep(0);
                }
            }
            aligned_free(wi);
        } else { // full content stage
            DbRecord existing;
            if(db_get_record_by_path(ctx->db, wi->parent_path, wi->name, &existing)){
                if(existing.type == DB_REC_FILE){
                    DbRecord r = existing;
                    wchar_t fpath[MAX_LONG_PATH];
                    _snwprintf(fpath, MAX_LONG_PATH, L"%s\\%s", wi->parent_path, wi->name);
                    if(wi->content){
                        r.content_str_id = db_intern_wstring(ctx->db, wi->content);
                        free(wi->content);
                    } else {
                        r.content_str_id = index_file_content(ctx->db, wi->parent_path, wi->name, &r.author_str_id, &r.title_str_id);
                    }
                    if(wi->preview){
                        r.preview_str_id = db_intern_wstring(ctx->db, wi->preview);
                        free(wi->preview);
                    } else if(needs_thumbnail(wi->name)){
                        wchar_t* thumb = GenerateThumbnail(fpath);
                        if(thumb){
                            r.preview_str_id = db_intern_wstring(ctx->db, thumb);
                            free(thumb);
                        }
                    }
                    extract_exif_metadata(ctx->db, fpath, &r);
                    extract_id3_metadata(ctx->db, fpath, &r);
                    if(is_archive_file(wi->name)){ index_archive(ctx->db, fpath); }
                    r.hash_crc = crc64_file(fpath);
                    buf[in_batch++] = r;
                }
            }
            aligned_free(wi);
        }
        if(in_batch >= (size_t)ctx->batch_size){
            if(!put_batch_with_growth(ctx, buf, in_batch)) { free(buf); return 1; }
            if(!db_commit_write(ctx->db)) { free(buf); return 1; }
            if(!db_begin_write(ctx->db))  { free(buf); return 1; }
            in_batch = 0;
        }
    }
    if(in_batch){
        if(!put_batch_with_growth(ctx, buf, in_batch)) { free(buf); return 1; }
        if(!db_commit_write(ctx->db)) { free(buf); return 1; }
    } else {
        db_commit_write(ctx->db);
    }
    free(buf);
    return 0;
}

// ---- CLI parsing ----
typedef struct {
    wchar_t dbPath[MAX_PATH];
    wchar_t rootPath[MAX_LONG_PATH];
    int threads;
    int batch;
    BOOL use_ntfs;
    BOOL tail_changes;
    BOOL all_drives;
} Args;

static void usage(void){
    wprintf(L"anything.exe index --db <path> (--root <folder> | --all-drives) [--threads N] [--batch N] [--ntfs] [--tail]\n");
    wprintf(L"anything.exe compress --db <path> --out <dest>\n");
}

static BOOL parse_args(int argc, wchar_t** argv, Args* a){
    ZeroMemory(a, sizeof(*a));
    a->threads = g_config.default_index_threads;
    a->batch = g_config.default_batch;
    a->use_ntfs=FALSE; a->tail_changes=FALSE; a->all_drives=FALSE;
    for(int i=1;i<argc;i++){
        if(wcscmp(argv[i], L"index")==0){ continue; }
        else if(wcscmp(argv[i], L"--db")==0 && i+1<argc){ wcscpy_s(a->dbPath, MAX_PATH, argv[++i]); }
        else if(wcscmp(argv[i], L"--root")==0 && i+1<argc){ wcscpy_s(a->rootPath, MAX_LONG_PATH, argv[++i]); }
        else if(wcscmp(argv[i], L"--threads")==0 && i+1<argc){ a->threads = _wtoi(argv[++i]); }
        else if(wcscmp(argv[i], L"--batch")==0 && i+1<argc){ a->batch = _wtoi(argv[++i]); }
        else if(wcscmp(argv[i], L"--ntfs")==0){ a->use_ntfs = TRUE; }
        else if(wcscmp(argv[i], L"--tail")==0){ a->tail_changes = TRUE; }
        else if(wcscmp(argv[i], L"--all-drives")==0){ a->all_drives = TRUE; }
        else { usage(); return FALSE; }
    }
    if(!a->dbPath[0] || (!a->rootPath[0] && !a->all_drives)){ usage(); return FALSE; }
    if(a->threads<1) a->threads=1;
    if(a->threads>g_config.max_index_threads) a->threads=g_config.max_index_threads;
    if(a->batch<1000) a->batch=1000;
    return TRUE;
}

static DWORD WINAPI scan_drive_thread(void* p){
    struct { wchar_t root[8]; WriterCtx* ctx; BOOL use_ntfs; int threads; } *in = p;
    (void)in->use_ntfs; // selection now handled internally
    FileScanner* fs = FileScanner_Start(in->root, in->threads, &in->ctx->queue, &in->ctx->cancel);
    if(fs){
        FileScanner_Wait(fs);
        FileScanner_Free(fs);
    }
    free(in);
    return 0;
}

int wmain(int argc, wchar_t** argv){
    config_init_default();
    config_load_file(L"anything.conf");
    enterprise_deploy_msi();
    if(argc>1 && wcscmp(argv[1], L"compress")==0){
        const wchar_t* dbPath=NULL; const wchar_t* outPath=NULL;
        for(int i=2;i<argc;i++){
            if(wcscmp(argv[i],L"--db")==0 && i+1<argc){ dbPath=argv[++i]; }
            else if(wcscmp(argv[i],L"--out")==0 && i+1<argc){ outPath=argv[++i]; }
            else { usage(); return 1; }
        }
        if(!dbPath || !outPath){ usage(); return 1; }
        Db* cdb=NULL;
        if(!db_open_readonly(dbPath, &cdb)){
            fwprintf(stderr, L"Failed to open DB at %s\n", dbPath);
            return 1;
        }
        BOOL ok = db_compress(cdb, outPath);
        if(!ok){
            const DbError* derr = db_last_error(cdb);
            fwprintf(stderr, L"Compression failed (err=%d: %hs)\n", derr->detail, derr->message);
        }
        db_close(cdb);
        return ok?0:1;
    }

    Args args;
    live_updates_init();
    if(!parse_args(argc, argv, &args)) return 1;
    enterprise_ad_authenticate("user", "");
    enterprise_index_network("\\\\networkshare");

    Db* db=NULL;
    if(!db_create(args.dbPath, /*init_mb*/1024, /*max_mb*/16384, &db)){
        const DbError* cerr = db ? db_last_error(db) : NULL;
        fwprintf(stderr, L"Failed to create/open DB at %s (err=%d: %hs)\n", args.dbPath, cerr?cerr->detail:0, cerr?cerr->message:"");
        return 1;
    }

    WriterCtx ctx = {0};
    ctx.db = db; ctx.batch_size = args.batch; ctx.done=FALSE; ctx.cancel.signaled = FALSE;
    if(!MPMC_Init(&ctx.queue, 1<<18)){
        fwprintf(stderr, L"MPMC_Init failed\n"); db_close(db); return 1;
    }
    PluginHost ph = { &ctx.queue, &ctx.cancel };
    Plugin_LoadAll(L"plugins", &ph);
    HANDLE writer = CreateThread(NULL,0,DbWriterThread,&ctx,0,NULL);

    HANDLE drive_threads[26]; int drive_count=0;
    // Incremental index state
    IndexState st={0}; BOOL has_state = db_get_index_state(db, &st);
    if(!has_state || st.indexing_level == 0) st.indexing_level = INDEX_FULL_CONTENT;
    uint8_t current_sigs[26][32];
    ZeroMemory(current_sigs, sizeof(current_sigs));
    for(int i=0;i<26;i++){
        wchar_t root[8];
        swprintf(root,8,L"%c:\\", L'A'+i);
        compute_drive_signature(root, current_sigs[i]);
    }

    if(args.all_drives){
        DWORD mask = GetLogicalDrives();
        for(int i=0;i<26;i++){
            if(!(mask & (1u<<i))) continue;
            if(has_state && memcmp(current_sigs[i], st.drive_signatures[i], 32) == 0) continue;
            wchar_t root[8]; swprintf(root, 8, L"%c:\\", L'A'+i);
            UINT type = GetDriveTypeW(root);
            if(type==DRIVE_FIXED || type==DRIVE_REMOVABLE){
                struct { wchar_t root[8]; WriterCtx* ctx; BOOL use_ntfs; int threads; } *in = (struct { wchar_t root[8]; WriterCtx* ctx; BOOL use_ntfs; int threads; }*)malloc(sizeof(*in));
                wcscpy_s(in->root, 8, root);
                in->ctx=&ctx; in->use_ntfs=args.use_ntfs; in->threads=args.threads;
                drive_threads[drive_count++] = CreateThread(NULL,0,scan_drive_thread,in,0,NULL);
            }
        }
    } else {
        int di=-1;
        if(args.rootPath[0] >= L'A' && args.rootPath[0] <= L'Z') di = args.rootPath[0]-L'A';
        else if(args.rootPath[0] >= L'a' && args.rootPath[0] <= L'z') di = args.rootPath[0]-L'a';
        BOOL need_scan = TRUE;
        if(di>=0 && di<26 && has_state){
            if(memcmp(current_sigs[di], st.drive_signatures[di], 32) == 0) need_scan = FALSE;
        }
        if(need_scan){
            struct { wchar_t root[8]; WriterCtx* ctx; BOOL use_ntfs; int threads; } *in = (struct { wchar_t root[8]; WriterCtx* ctx; BOOL use_ntfs; int threads; }*)malloc(sizeof(*in));
            wcscpy_s(in->root, 8, args.rootPath);
            in->ctx=&ctx; in->use_ntfs=args.use_ntfs; in->threads=args.threads;
            drive_threads[drive_count++] = CreateThread(NULL,0,scan_drive_thread,in,0,NULL);
        }
    }

    WaitForMultipleObjects(drive_count, drive_threads, TRUE, INFINITE);
    for(int i=0;i<drive_count;i++) CloseHandle(drive_threads[i]);

    Plugin_ScanAll();

    if(args.tail_changes && !args.all_drives){
        HANDLE tailer = StartUSNTailer(args.rootPath, &ctx.queue, &ctx.cancel);
        if(tailer){
            wprintf(L"Tailing changes on %s. Press Ctrl+C to stop...\n", args.rootPath);
            Sleep(5000); // demo
        }
    }

    ctx.done = TRUE;
    MPMC_Push(&ctx.queue, NULL); // sentinel
    WaitForSingleObject(writer, INFINITE);
    CloseHandle(writer);

    memcpy(st.drive_signatures, current_sigs, sizeof(current_sigs));
    FILETIME now; GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER uli; uli.LowPart = now.dwLowDateTime; uli.HighPart = now.dwHighDateTime;
    st.last_scan_time = uli.QuadPart;
    if(db_begin_write(db)){
        db_set_index_state(db, &st);
        db_commit_write(db);
    }

    const DbHeader* header = db_header(db);
    if(header){
        wprintf(L"Total records: %llu, strings: %llu, map: %llu MiB\n",
            (unsigned long long)header->record_count,
            (unsigned long long)header->string_count,
            (unsigned long long)(header->map_size_bytes/1024/1024));
    }
    Plugin_UnloadAll();
    db_close(db);
    MPMC_Destroy(&ctx.queue);
    return 0;
}
