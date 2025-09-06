// Microsoft Mail Scanner Plugin
// Integrates with Microsoft Graph API using OAuth 2.0 access tokens.

#include "plugin.h"
#include <wchar.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <curl/curl.h>
#include "cJSON.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#define WAIT_OBJECT_0 0
static int WaitForSingleObject(HANDLE h,unsigned int ms){(void)h;(void)ms;return 1;}
#define Sleep(ms) usleep((ms)*1000)
static int wcscpy_s(wchar_t* dst,size_t dstcch,const wchar_t* src){ if(!dst||!src||dstcch==0) return 1; wcsncpy(dst,src,dstcch); dst[dstcch-1]=0; return 0; }
#endif

static void to_utf8(const wchar_t* w,char* u8,size_t cap){
    if(!w||!u8||cap==0){ if(u8) u8[0]=0; return; }
#ifdef _WIN32
    WideCharToMultiByte(CP_UTF8,0,w,-1,u8,(int)cap,NULL,NULL);
#else
    wcstombs(u8,w,cap);
#endif
    u8[cap-1]=0;
}

static void to_wide(const char* u8,wchar_t* w,size_t cap){
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

static uint64_t parse_rfc3339(const char* s){
    int Y,M,D,h,m; float sf;
    if(!s||sscanf(s,"%d-%d-%dT%d:%d:%f",&Y,&M,&D,&h,&m,&sf)!=6) return 0;
    struct tm tm={0};
    tm.tm_year=Y-1900; tm.tm_mon=M-1; tm.tm_mday=D;
    tm.tm_hour=h; tm.tm_min=m; tm.tm_sec=(int)sf;
#ifdef _WIN32
    time_t t=_mkgmtime(&tm);
#else
    time_t t=timegm(&tm);
#endif
    if(t<0) return 0;
    return (uint64_t)t;
}

static uint64_t to_filetime(uint64_t unix_secs){
    return unix_secs*10000000ULL + 116444736000000000ULL;
}

struct curl_buf{ char* data; size_t size; };
static size_t curl_write_cb(void* contents,size_t size,size_t nmemb,void* userp){
    size_t realsize=size*nmemb; struct curl_buf* mem=(struct curl_buf*)userp;
    char* ptr=(char*)realloc(mem->data, mem->size+realsize+1);
    if(!ptr){
        free(mem->data);
        mem->data=NULL;
        mem->size=0;
        return 0;
    }
    mem->data=ptr; memcpy(&mem->data[mem->size],contents,realsize);
    mem->size+=realsize; mem->data[mem->size]=0; return realsize;
}

static BOOL http_get(const char* url,const char* token,char** out){
    if(out) *out=NULL; static int curl_inited=0;
    if(!curl_inited){ if(curl_global_init(CURL_GLOBAL_DEFAULT)!=0) return FALSE; curl_inited=1; }
    CURL* curl=curl_easy_init(); if(!curl) return FALSE;
    curl_easy_setopt(curl,CURLOPT_URL,url);
    struct curl_slist* hdr=NULL; char auth[512];
    snprintf(auth,sizeof(auth),"Authorization: Bearer %s",token);
    hdr=curl_slist_append(hdr,auth); curl_easy_setopt(curl,CURLOPT_HTTPHEADER,hdr);
    struct curl_buf buf={0};
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,curl_write_cb);
    curl_easy_setopt(curl,CURLOPT_WRITEDATA,&buf);
    CURLcode res=curl_easy_perform(curl);
    if(hdr) curl_slist_free_all(hdr); curl_easy_cleanup(curl);
    if(res!=CURLE_OK){ free(buf.data); return FALSE; }
    if(out) *out=buf.data; else free(buf.data); return TRUE;
}

static wchar_t g_token[256]=L"";
static PluginHost g_host;

static BOOL load_token(void){
    const char* env=getenv("MS_MAIL_TOKEN");
    if(env){ to_wide(env,g_token,256); return TRUE; }
    const char* path_env=getenv("MS_MAIL_TOKEN_FILE");
    char path_u8[512];
    if(path_env){ strncpy(path_u8,path_env,sizeof(path_u8)-1); path_u8[sizeof(path_u8)-1]=0; }
    else {
        const char* home=getenv("HOME");
#ifdef _WIN32
        if(!home) return FALSE; snprintf(path_u8,sizeof(path_u8),"%s\\.anything\\ms_mail_token",home);
#else
        if(!home) return FALSE; snprintf(path_u8,sizeof(path_u8),"%s/.anything/ms_mail_token",home);
#endif
    }
    FILE* f=fopen(path_u8,"rb");
    if(!f){ fprintf(stderr,"[msmail] failed to open token file %s: %s\n",path_u8,strerror(errno)); return FALSE; }
#ifndef _WIN32
    struct stat st; if(stat(path_u8,&st)!=0){ fprintf(stderr,"[msmail] stat failed for %s: %s\n",path_u8,strerror(errno)); fclose(f); return FALSE; }
    if((st.st_mode&0777)!=0600){ fprintf(stderr,"[msmail] insecure permissions on %s\n",path_u8); fclose(f); return FALSE; }
#endif
    char buf[256]; size_t n=fread(buf,1,sizeof(buf)-1,f); fclose(f); if(n==0){ fprintf(stderr,"[msmail] token file %s empty or unreadable\n",path_u8); return FALSE; }
    buf[n]=0; char* nl=strpbrk(buf,"\r\n"); if(nl) *nl=0;
    to_wide(buf,g_token,256);
    return g_token[0]!=L'\0';
}

static void process_message(const char* id,const char* token_utf8){
    char url[512]; snprintf(url,sizeof(url),"https://graph.microsoft.com/v1.0/me/messages/%s?$select=subject,bodyPreview,receivedDateTime",id);
    char* resp=NULL; if(!http_get(url,token_utf8,&resp)) return;
    cJSON* root=cJSON_Parse(resp); free(resp); if(!root) return;
    const char* subject=NULL; const char* preview=""; const char* received=NULL;
    cJSON* sj=cJSON_GetObjectItem(root,"subject"); if(cJSON_IsString(sj)) subject=sj->valuestring;
    cJSON* pv=cJSON_GetObjectItem(root,"bodyPreview"); if(cJSON_IsString(pv)) preview=pv->valuestring;
    cJSON* rd=cJSON_GetObjectItem(root,"receivedDateTime"); if(cJSON_IsString(rd)) received=rd->valuestring;
    uint64_t ts=received? parse_rfc3339(received):0;
    wchar_t wname[MAX_PATH]; if(subject) to_wide(subject,wname,MAX_PATH); else to_wide(id,wname,MAX_PATH);
    size_t wlen=mbstowcs(NULL,preview,0); wchar_t* wcontent=NULL;
    if(wlen!=(size_t)-1){ wcontent=(wchar_t*)malloc((wlen+1)*sizeof(wchar_t)); if(wcontent) mbstowcs(wcontent,preview,wlen+1); }
    DbWorkItem* wi=(DbWorkItem*)wi_alloc(); if(!wi){ if(wcontent) free(wcontent); cJSON_Delete(root); return; }
    wcscpy_s(wi->parent_path,MAX_LONG_PATH,L"msmail");
    wcscpy_s(wi->name,MAX_PATH,wname);
    uint64_t ft=ts? to_filetime(ts):0;
    wi->file_size=0; wi->creation_time=ft; wi->modified_time=ft; wi->access_time=ft;
    wi->attributes=0; wi->stage=INDEX_FULL_CONTENT; wi->op=WI_ADD;
    wi->content=wcontent; wi->preview=NULL; wi->clone_id=0;
    int tries=0; while(!MPMC_Push(g_host.queue,wi)){
        if(WaitForSingleObject(g_host.cancel_event,0)==WAIT_OBJECT_0 || tries++>1000){ if(wcontent) free(wcontent); wi_free(wi); cJSON_Delete(root); return; }
        Sleep(0);
    }
    cJSON_Delete(root);
}

static BOOL init(const PluginHost* host){
    if(!host) return FALSE; g_host=*host;
    if(!load_token()) return TRUE; // token may be absent
    const char* store_path=getenv("MS_MAIL_TOKEN_STORE");
    if(store_path){
        char tok_u8[256]; to_utf8(g_token,tok_u8,sizeof(tok_u8));
        FILE* f=fopen(store_path,"wb");
        if(!f){
            fprintf(stderr,"[msmail] cannot write token store %s: %s\n",store_path,strerror(errno));
        }else{
#ifndef _WIN32
            fchmod(fileno(f),0600);
#endif
            fwrite(tok_u8,1,strlen(tok_u8),f); fclose(f);
        }
    }
    return TRUE;
}

static void scan(void){
    if(g_token[0]==L'\0') return; char token_utf8[256];
    to_utf8(g_token,token_utf8,sizeof(token_utf8));
    char* resp=NULL; if(!http_get("https://graph.microsoft.com/v1.0/me/messages?$top=5",token_utf8,&resp)) return;
    cJSON* root=cJSON_Parse(resp); free(resp); if(!root) return;
    cJSON* arr=cJSON_GetObjectItem(root,"value");
    if(cJSON_IsArray(arr)){
        cJSON* m=NULL; cJSON_ArrayForEach(m,arr){
            cJSON* id=cJSON_GetObjectItem(m,"id");
            if(cJSON_IsString(id)) process_message(id->valuestring,token_utf8);
            if(WaitForSingleObject(g_host.cancel_event,0)==WAIT_OBJECT_0) break;
        }
    }
    cJSON_Delete(root);
}

static void msmail_shutdown(void){
    // no persistent resources
}

static AnythingPlugin g_plugin={
    ANYTHING_PLUGIN_API_VERSION,
    L"Microsoft Mail Scanner",
    init,
    scan,
    msmail_shutdown
};

#ifdef _WIN32
__declspec(dllexport)
#endif
AnythingPlugin* Anything_GetPlugin(void){ return &g_plugin; }

