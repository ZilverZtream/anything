// Web Archive Scanner Plugin
// Indexes full text of visited or bookmarked web pages from browsers.

#include "anything/plugin.h"
#include "../../third_party/cJSON/cJSON.h"
#include <wchar.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <sqlite3.h>
#include <curl/curl.h>
#include <limits.h>
#include <gumbo.h>
#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include <direct.h>
#else
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <strings.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
char* strdup(const char*);
int posix_memalign(void**, size_t, size_t);
int usleep(unsigned int);
#define Sleep(ms) usleep((ms)*1000)
static int wcscpy_s(wchar_t* dst,size_t dstcch,const wchar_t* src){if(!dst||!src||dstcch==0) return 1; wcsncpy(dst,src,dstcch); dst[dstcch-1]=0; return 0;}
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#ifdef _WIN32
#define strncasecmp _strnicmp
#define strdup _strdup
#endif

static PluginHost g_host;

#ifdef _WIN32
static HANDLE g_mutex;
#else
static pthread_mutex_t g_mutex;
#endif

// helper conversions and aligned allocation
static void to_utf8(const wchar_t* w, char* u8, size_t cap){
    if(!w||!u8||cap==0){ if(u8) u8[0]=0; return; }
#ifdef _WIN32
    WideCharToMultiByte(CP_UTF8,0,w,-1,u8,(int)cap,NULL,NULL);
#else
    wcstombs(u8,w,cap);
#endif
    u8[cap-1]=0;
}

static void to_wide(const char* u8, wchar_t* w, size_t cap){
    if(!u8||!w||cap==0){ if(w) w[0]=0; return; }
#ifdef _WIN32
    MultiByteToWideChar(CP_UTF8,0,u8,-1,w,(int)cap);
#else
    mbstowcs(w,u8,cap);
#endif
    w[cap-1]=0;
}

static void* wi_alloc(void){
#ifdef _WIN32
    return _aligned_malloc(sizeof(DbWorkItem), CACHE_LINE_SIZE);
#else
    void* p=NULL; if(posix_memalign(&p,CACHE_LINE_SIZE,sizeof(DbWorkItem))!=0) return NULL; return p;
#endif
}

static void wi_free(void* p){
#ifdef _WIN32
    _aligned_free(p);
#else
    free(p);
#endif
}

// ---- URL seen tracking ----
typedef struct{char* url;} Seen;
static Seen* g_seen=NULL; static size_t g_seen_count=0,g_seen_cap=0;
static char g_state_file[PATH_MAX];

static void seen_add_mem(const char* url){
    if(!url) return;
    for(size_t i=0;i<g_seen_count;i++) if(strcmp(g_seen[i].url,url)==0) return;
    if(g_seen_count==g_seen_cap){
        size_t nc=g_seen_cap?g_seen_cap*2:128;
        Seen* tmp=(Seen*)realloc(g_seen,nc*sizeof(Seen));
        if(!tmp) return; g_seen=tmp; g_seen_cap=nc;
    }
    g_seen[g_seen_count].url=strdup(url); g_seen_count++;
}

static void seen_add(const char* url){
    if(!url) return; seen_add_mem(url);
    FILE* f=fopen(g_state_file,"a");
    if(!f){ fprintf(stderr,"[web_archive] failed to open %s for append: %s\n",g_state_file,strerror(errno)); }
    else { fprintf(f,"%s\n",url); fclose(f); }
}

static int seen_has(const char* url){
    if(!url) return 1; for(size_t i=0;i<g_seen_count;i++) if(strcmp(g_seen[i].url,url)==0) return 1; return 0; }

static void load_state(void){
    FILE* f=fopen(g_state_file,"r");
    if(!f){ fprintf(stderr,"[web_archive] unable to open state file %s: %s\n",g_state_file,strerror(errno)); return; }
    char line[4096];
    while(fgets(line,sizeof(line),f)){
        size_t len=strlen(line); while(len&&(line[len-1]=='\n'||line[len-1]=='\r')) line[--len]=0;
        if(len>0) seen_add_mem(line);
    }
    fclose(f);
}

static void init_state_path(void){
#ifdef _WIN32
    char app[MAX_PATH];
    if(SHGetFolderPathA(NULL,CSIDL_LOCAL_APPDATA,NULL,0,app)==S_OK){
        _snprintf(g_state_file,sizeof(g_state_file),"%s\\Anything",app);
        _mkdir(g_state_file); // ensure dir
        _snprintf(g_state_file,sizeof(g_state_file),"%s\\Anything\\web_archive_state.txt",app);
    }else{
        strncpy(g_state_file,"web_archive_state.txt",sizeof(g_state_file));
    }
#else
    const char* home=getenv("HOME");
    if(home) snprintf(g_state_file,sizeof(g_state_file),"%s/.web_archive_state",home);
    else strncpy(g_state_file,"web_archive_state",sizeof(g_state_file));
#endif
}

// ---- HTTP fetch and HTML parsing ----
struct curl_buf{ char* data; size_t size; };
static size_t curl_write_cb(void* contents,size_t sz,size_t nmemb,void* userp){
    size_t realsz=sz*nmemb; struct curl_buf* mem=(struct curl_buf*)userp;
    char* ptr=(char*)realloc(mem->data,mem->size+realsz+1);
    if(!ptr){
        free(mem->data);
        mem->data=NULL;
        mem->size=0;
        return 0;
    }
    mem->data=ptr; memcpy(&mem->data[mem->size],contents,realsz);
    mem->size+=realsz; mem->data[mem->size]=0; return realsz; }

static char* http_fetch(const char* url){
    if(!url) return NULL; static int inited=0; if(!inited){ curl_global_init(CURL_GLOBAL_DEFAULT); inited=1; }
    CURL* curl=curl_easy_init(); if(!curl) return NULL; struct curl_buf buf={0};
    curl_easy_setopt(curl,CURLOPT_URL,url); curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,1L);
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,curl_write_cb); curl_easy_setopt(curl,CURLOPT_WRITEDATA,&buf);
    CURLcode res=curl_easy_perform(curl); curl_easy_cleanup(curl);
    if(res!=CURLE_OK){ free(buf.data); return NULL; } return buf.data;
}

static void append_text(GumboNode* node,char** out,size_t* len,size_t* cap){
    if(node->type==GUMBO_NODE_TEXT){
        const char* text=node->v.text.text;
        size_t tlen=strlen(text);
        if(*len + tlen + 1 > *cap){
            size_t ncap=*cap? *cap*2 + tlen: tlen+1;
            char* tmp=(char*)realloc(*out,ncap);
            if(!tmp) return;
            *out=tmp; *cap=ncap;
        }
        for(size_t i=0;i<tlen;i++){
            char c=text[i];
            if(c=='\n'||c=='\r') c=' ';
            (*out)[(*len)++]=c;
        }
        (*out)[*len]=0;
    }else if(node->type==GUMBO_NODE_ELEMENT &&
             node->v.element.tag!=GUMBO_TAG_SCRIPT &&
             node->v.element.tag!=GUMBO_TAG_STYLE){
        GumboVector* children=&node->v.element.children;
        for(size_t i=0;i<children->length;i++){
            append_text(children->data[i],out,len,cap);
        }
    }
}

static char* html_to_text(const char* html){
    if(!html) return NULL;
    GumboOutput* output=gumbo_parse(html);
    if(!output) return NULL;
    size_t cap=256,len=0;
    char* out=(char*)malloc(cap);
    if(!out){ gumbo_destroy_output(&kGumboDefaultOptions,output); return NULL; }
    out[0]=0;
    append_text(output->root,&out,&len,&cap);
    gumbo_destroy_output(&kGumboDefaultOptions,output);
    return out;
}

static void push_page(const char* url,const char* title,const char* text){
    DbWorkItem* wi=(DbWorkItem*)wi_alloc(); if(!wi) return;
    wchar_t wurl[MAX_LONG_PATH]; to_wide(url,wurl,MAX_LONG_PATH);
    wchar_t wtitle[MAX_PATH]; if(title && title[0]) to_wide(title,wtitle,MAX_PATH); else to_wide(url,wtitle,MAX_PATH);
    wcscpy_s(wi->parent_path,MAX_LONG_PATH,wurl); wcscpy_s(wi->name,MAX_PATH,wtitle);
    wi->file_size=0; wi->creation_time=0; wi->modified_time=0; wi->access_time=0; wi->attributes=0;
    wi->stage=INDEX_FULL_CONTENT; wi->op=WI_ADD; wi->preview=NULL; wi->clone_id=0;
    wchar_t* wcontent=NULL;
    if(text){ size_t wlen=mbstowcs(NULL,text,0); if(wlen!=(size_t)-1){ wcontent=(wchar_t*)malloc((wlen+1)*sizeof(wchar_t)); if(wcontent) mbstowcs(wcontent,text,wlen+1); } }
    wi->content=wcontent;
    int tries=0; while(!MPMC_Push(g_host.queue,wi)){
        if(is_cancelled(g_host.cancel_token) || tries++>1000){ if(wcontent) free(wcontent); wi_free(wi); return; }
        Sleep(0); }
}

static void process_url(const char* url,const char* title){
    if(!url||seen_has(url)) return; char* html=http_fetch(url); if(!html){ seen_add(url); return; }
    char* text=html_to_text(html); free(html); if(text){ push_page(url,title,text); free(text); }
    seen_add(url);
}

// ---- Chrome ----
static void parse_chrome_bm(cJSON* node){
    if(!node) return; cJSON* url=cJSON_GetObjectItem(node,"url"); cJSON* name=cJSON_GetObjectItem(node,"name");
    if(cJSON_IsString(url)) process_url(url->valuestring, cJSON_IsString(name)?name->valuestring:NULL);
    cJSON* children=cJSON_GetObjectItem(node,"children"); if(cJSON_IsArray(children)){
        cJSON* ch=NULL; cJSON_ArrayForEach(ch,children){ parse_chrome_bm(ch); }
    }
}

static void scan_chromium_variant(const char* hist,const char* bm){
    sqlite3* db=NULL; if(sqlite3_open(hist,&db)==SQLITE_OK){
        const char* sql="SELECT url,title FROM urls"; sqlite3_stmt* stmt=NULL;
        if(sqlite3_prepare_v2(db,sql,-1,&stmt,NULL)==SQLITE_OK){
            while(sqlite3_step(stmt)==SQLITE_ROW){
                const unsigned char* u=sqlite3_column_text(stmt,0);
                const unsigned char* t=sqlite3_column_text(stmt,1);
                process_url((const char*)u,(const char*)t);
                if(is_cancelled(g_host.cancel_token)) break;
            }
            sqlite3_finalize(stmt);
        }
        sqlite3_close(db);
    }
    FILE* fb=fopen(bm,"rb");
    if(!fb){
        fprintf(stderr,"[web_archive] failed to open %s: %s\n",bm,strerror(errno));
    }else{
        fseek(fb,0,SEEK_END); long sz=ftell(fb); fseek(fb,0,SEEK_SET);
        char* data=(char*)malloc(sz+1);
        if(data){
            if(fread(data,1,sz,fb)!=(size_t)sz){
                fprintf(stderr,"[web_archive] failed to read %s\n",bm);
                free(data); fclose(fb); return;
            }
            data[sz]=0; cJSON* root=cJSON_Parse(data);
            if(root){
                cJSON* roots=cJSON_GetObjectItem(root,"roots");
                if(roots){ cJSON* child=roots->child; while(child){ parse_chrome_bm(child); child=child->next; } }
                cJSON_Delete(root);
            }
            free(data);
        }
        fclose(fb);
    }
}

static void scan_chrome(void){
    char hist[PATH_MAX]; char bm[PATH_MAX]; hist[0]=0; bm[0]=0;
#ifdef _WIN32
    char* local=getenv("LOCALAPPDATA"); if(!local) return;
    snprintf(hist,sizeof(hist),"%s\\Google\\Chrome\\User Data\\Default\\History",local);
    snprintf(bm,sizeof(bm),"%s\\Google\\Chrome\\User Data\\Default\\Bookmarks",local);
#elif defined(__APPLE__)
    const char* home=getenv("HOME"); if(!home) return;
    snprintf(hist,sizeof(hist),"%s/Library/Application Support/Google/Chrome/Default/History",home);
    snprintf(bm,sizeof(bm),"%s/Library/Application Support/Google/Chrome/Default/Bookmarks",home);
#else
    const char* home=getenv("HOME"); if(!home) return;
    snprintf(hist,sizeof(hist),"%s/.config/google-chrome/Default/History",home);
    snprintf(bm,sizeof(bm),"%s/.config/google-chrome/Default/Bookmarks",home);
#endif
    scan_chromium_variant(hist,bm);
}

static void scan_edge(void){
    char hist[PATH_MAX]; char bm[PATH_MAX]; hist[0]=0; bm[0]=0;
#ifdef _WIN32
    char* local=getenv("LOCALAPPDATA"); if(!local) return;
    snprintf(hist,sizeof(hist),"%s\\Microsoft\\Edge\\User Data\\Default\\History",local);
    snprintf(bm,sizeof(bm),"%s\\Microsoft\\Edge\\User Data\\Default\\Bookmarks",local);
#elif defined(__APPLE__)
    const char* home=getenv("HOME"); if(!home) return;
    snprintf(hist,sizeof(hist),"%s/Library/Application Support/Microsoft Edge/Default/History",home);
    snprintf(bm,sizeof(bm),"%s/Library/Application Support/Microsoft Edge/Default/Bookmarks",home);
#else
    const char* home=getenv("HOME"); if(!home) return;
    snprintf(hist,sizeof(hist),"%s/.config/microsoft-edge/Default/History",home);
    snprintf(bm,sizeof(bm),"%s/.config/microsoft-edge/Default/Bookmarks",home);
#endif
    scan_chromium_variant(hist,bm);
}

static void scan_opera(void){
    char hist[PATH_MAX]; char bm[PATH_MAX]; hist[0]=0; bm[0]=0;
#ifdef _WIN32
    char* app=getenv("APPDATA"); if(!app) return;
    snprintf(hist,sizeof(hist),"%s\\Opera Software\\Opera Stable\\History",app);
    snprintf(bm,sizeof(bm),"%s\\Opera Software\\Opera Stable\\Bookmarks",app);
#elif defined(__APPLE__)
    const char* home=getenv("HOME"); if(!home) return;
    snprintf(hist,sizeof(hist),"%s/Library/Application Support/com.operasoftware.Opera/History",home);
    snprintf(bm,sizeof(bm),"%s/Library/Application Support/com.operasoftware.Opera/Bookmarks",home);
#else
    const char* home=getenv("HOME"); if(!home) return;
    snprintf(hist,sizeof(hist),"%s/.config/opera/History",home);
    snprintf(bm,sizeof(bm),"%s/.config/opera/Bookmarks",home);
#endif
    scan_chromium_variant(hist,bm);
}

static void scan_opera_gx(void){
    char hist[PATH_MAX]; char bm[PATH_MAX]; hist[0]=0; bm[0]=0;
#ifdef _WIN32
    char* app=getenv("APPDATA"); if(!app) return;
    snprintf(hist,sizeof(hist),"%s\\Opera Software\\Opera GX Stable\\History",app);
    snprintf(bm,sizeof(bm),"%s\\Opera Software\\Opera GX Stable\\Bookmarks",app);
#elif defined(__APPLE__)
    const char* home=getenv("HOME"); if(!home) return;
    snprintf(hist,sizeof(hist),"%s/Library/Application Support/com.operasoftware.OperaGX/History",home);
    snprintf(bm,sizeof(bm),"%s/Library/Application Support/com.operasoftware.OperaGX/Bookmarks",home);
#else
    const char* home=getenv("HOME"); if(!home) return;
    snprintf(hist,sizeof(hist),"%s/.config/opera-gx/History",home);
    snprintf(bm,sizeof(bm),"%s/.config/opera-gx/Bookmarks",home);
#endif
    scan_chromium_variant(hist,bm);
}

// ---- Firefox ----
static void scan_firefox_db(const char* path){
    sqlite3* db=NULL; if(sqlite3_open(path,&db)!=SQLITE_OK) return; const char* sql="SELECT url,title FROM moz_places";
    sqlite3_stmt* stmt=NULL; if(sqlite3_prepare_v2(db,sql,-1,&stmt,NULL)==SQLITE_OK){
        while(sqlite3_step(stmt)==SQLITE_ROW){ const unsigned char* u=sqlite3_column_text(stmt,0); const unsigned char* t=sqlite3_column_text(stmt,1); process_url((const char*)u,(const char*)t); if(is_cancelled(g_host.cancel_token)) break; }
        sqlite3_finalize(stmt); }
    sqlite3_close(db);
}

static void scan_firefox(void){
    char prof[PATH_MAX];
#ifdef _WIN32
    char* app=getenv("APPDATA"); if(!app) return; snprintf(prof,sizeof(prof),"%s\\Mozilla\\Firefox\\Profiles",app);
    wchar_t wprof[MAX_PATH]; to_wide(prof,wprof,MAX_PATH); wchar_t pattern[MAX_PATH]; _snwprintf(pattern,MAX_PATH,L"%s\\*",wprof);
    WIN32_FIND_DATAW fd; HANDLE h=FindFirstFileW(pattern,&fd); if(h==INVALID_HANDLE_VALUE) return; do{
        if(!(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)) continue; if(fd.cFileName[0]==L'.') continue; wchar_t dbp[MAX_PATH];
        _snwprintf(dbp,MAX_PATH,L"%s\\%s\\places.sqlite",wprof,fd.cFileName); char path_u8[PATH_MAX]; to_utf8(dbp,path_u8,sizeof(path_u8));
        struct _stat st; if(_stat(path_u8,&st)==0) scan_firefox_db(path_u8);
    }while(FindNextFileW(h,&fd)); FindClose(h);
#elif defined(__APPLE__)
    const char* home=getenv("HOME"); if(!home) return; snprintf(prof,sizeof(prof),"%s/Library/Application Support/Firefox/Profiles",home);
    DIR* d=opendir(prof); if(!d) return; struct dirent* ent; while((ent=readdir(d))){ if(ent->d_name[0]=='.') continue; char dbp[PATH_MAX]; snprintf(dbp,sizeof(dbp),"%s/%s/places.sqlite",prof,ent->d_name); struct stat st; if(stat(dbp,&st)==0) scan_firefox_db(dbp); } closedir(d);
#else
    const char* home=getenv("HOME"); if(!home) return; snprintf(prof,sizeof(prof),"%s/.mozilla/firefox",home);
    DIR* d=opendir(prof); if(!d) return; struct dirent* ent; while((ent=readdir(d))){ if(ent->d_name[0]=='.') continue; char dbp[PATH_MAX]; snprintf(dbp,sizeof(dbp),"%s/%s/places.sqlite",prof,ent->d_name); struct stat st; if(stat(dbp,&st)==0) scan_firefox_db(dbp); } closedir(d);
#endif
}

// ---- Safari (macOS only) ----
#ifdef __APPLE__
static void parse_safari_bookmarks(const char* path){
    FILE* f=fopen(path,"rb");
    if(!f){ fprintf(stderr,"[web_archive] failed to open %s: %s\n",path,strerror(errno)); return; }
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    char* data=(char*)malloc(sz+1);
    if(!data){ fclose(f); return; }
    if(fread(data,1,sz,f)!=(size_t)sz){ fprintf(stderr,"[web_archive] failed to read %s\n",path); free(data); fclose(f); return; }
    data[sz]=0; fclose(f);
    char* p=data; while((p=strstr(p,"<key>URLString</key>"))){ char* s=strstr(p,"<string>"); if(!s) break; s+=8; char* e=strstr(s,"</string>"); if(!e) break; char url[4096]; size_t l=e-s; if(l>4095) l=4095; memcpy(url,s,l); url[l]=0; char* tkey=strstr(p,"<key>title</key>"); const char* title=NULL; char tbuf[1024]; if(tkey && tkey<e){ char* ts=strstr(tkey,"<string>"); if(ts){ ts+=8; char* te=strstr(ts,"</string>"); if(te){ size_t tl=te-ts; if(tl>1023) tl=1023; memcpy(tbuf,ts,tl); tbuf[tl]=0; title=tbuf; } }} process_url(url,title); p=e; }
    free(data);
}

static void scan_safari(void){
    const char* home=getenv("HOME"); if(!home) return; char hist[PATH_MAX]; snprintf(hist,sizeof(hist),"%s/Library/Safari/History.db",home);
    sqlite3* db=NULL; if(sqlite3_open(hist,&db)==SQLITE_OK){ const char* sql="SELECT url,title FROM history_items"; sqlite3_stmt* stmt=NULL; if(sqlite3_prepare_v2(db,sql,-1,&stmt,NULL)==SQLITE_OK){ while(sqlite3_step(stmt)==SQLITE_ROW){ const unsigned char* u=sqlite3_column_text(stmt,0); const unsigned char* t=sqlite3_column_text(stmt,1); process_url((const char*)u,(const char*)t); if(is_cancelled(g_host.cancel_token)) break; } sqlite3_finalize(stmt); } sqlite3_close(db); }
    char bm[PATH_MAX]; snprintf(bm,sizeof(bm),"%s/Library/Safari/Bookmarks.plist",home); parse_safari_bookmarks(bm);
}
#endif

// ---- Plugin entry points ----
static BOOL init(const PluginHost* host){
    if(!host) return FALSE;
    g_host = *host;
#ifdef _WIN32
    if(!g_mutex) g_mutex = CreateMutex(NULL, FALSE, NULL);
#else
    pthread_mutex_init(&g_mutex, NULL);
#endif
    init_state_path();
    load_state();
    return TRUE;
}

static void scan(void){
#ifdef _WIN32
    WaitForSingleObject(g_mutex, INFINITE);
#else
    pthread_mutex_lock(&g_mutex);
#endif

    scan_chrome();
    scan_edge();
    scan_opera();
    scan_opera_gx();
    scan_firefox();
#ifdef __APPLE__
    scan_safari();
#endif

#ifdef _WIN32
    ReleaseMutex(g_mutex);
#else
    pthread_mutex_unlock(&g_mutex);
#endif
}

static void plugin_shutdown(void){
#ifdef _WIN32
    WaitForSingleObject(g_mutex, INFINITE);
#else
    pthread_mutex_lock(&g_mutex);
#endif
    for(size_t i=0;i<g_seen_count;i++) free(g_seen[i].url);
    free(g_seen);
#ifdef _WIN32
    ReleaseMutex(g_mutex);
    CloseHandle(g_mutex);
    g_mutex = NULL;
#else
    pthread_mutex_unlock(&g_mutex);
    pthread_mutex_destroy(&g_mutex);
#endif
}

static AnythingPlugin g_plugin={ ANYTHING_PLUGIN_API_VERSION, L"Web Archive Scanner", init, scan, plugin_shutdown };

#ifdef _WIN32
__declspec(dllexport)
#endif
AnythingPlugin* Anything_GetPlugin(void){ return &g_plugin; }

