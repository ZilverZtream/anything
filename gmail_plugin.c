// Gmail Scanner Plugin
// Integrates with the Gmail REST API using OAuth 2.0 access tokens.

#include "plugin.h"
#include <wchar.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <curl/curl.h>
#include "cJSON.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#define WAIT_OBJECT_0 0
static int WaitForSingleObject(HANDLE h, unsigned int ms){(void)h;(void)ms;return 1;}
#define Sleep(ms) usleep((ms)*1000)
static int wcscpy_s(wchar_t* dst,size_t dstcch,const wchar_t* src){
    if(!dst||!src||dstcch==0) return 1; wcsncpy(dst,src,dstcch); dst[dstcch-1]=0; return 0;
}
#endif

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
    void* p=NULL; if(posix_memalign(&p, CACHE_LINE_SIZE, sizeof(DbWorkItem))!=0) return NULL; return p;
#endif
}

static void wi_free(void* p){
#ifdef _WIN32
    _aligned_free(p);
#else
    free(p);
#endif
}

static wchar_t g_oauth_token[256] = L"";
static PluginHost g_host;

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

static BOOL http_get(const char* url, const char* token, char** out){
    if(out) *out=NULL; static int curl_inited=0;
    if(!curl_inited){
        if(curl_global_init(CURL_GLOBAL_DEFAULT)!=0){
            fprintf(stderr,"[gmail] curl_global_init failed\n");
            return FALSE;
        }
        curl_inited=1;
    }
    CURL* curl=curl_easy_init();
    if(!curl){
        fprintf(stderr,"[gmail] curl_easy_init failed\n");
        return FALSE;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    struct curl_slist* hdr=NULL; char auth[512];
    snprintf(auth,sizeof(auth),"Authorization: Bearer %s",token);
    hdr=curl_slist_append(hdr,auth); curl_easy_setopt(curl,CURLOPT_HTTPHEADER,hdr);
    struct curl_buf buf={0};
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,curl_write_cb);
    curl_easy_setopt(curl,CURLOPT_WRITEDATA,&buf);
    CURLcode res=curl_easy_perform(curl);
    if(hdr) curl_slist_free_all(hdr); curl_easy_cleanup(curl);
    if(res!=CURLE_OK){
        fprintf(stderr,"[gmail] curl_easy_perform failed: %s\n", curl_easy_strerror(res));
        free(buf.data);
        return FALSE;
    }
    if(out) *out=buf.data; else free(buf.data); return TRUE;
}

static uint64_t to_filetime(uint64_t unix_secs){
    return unix_secs*10000000ULL + 116444736000000000ULL;
}

static void process_message(const char* id, const char* token_utf8){
    char url[512]; snprintf(url,sizeof(url),"https://gmail.googleapis.com/gmail/v1/users/me/messages/%s?format=full",id);
    char* resp=NULL; cJSON* root=NULL; wchar_t* wcontent=NULL; DbWorkItem* wi=NULL;
    if(!http_get(url,token_utf8,&resp)){
        fprintf(stderr,"[gmail] failed to fetch message %s\n", id);
        goto cleanup;
    }
    root=cJSON_Parse(resp);
    if(!root){
        fprintf(stderr,"[gmail] failed to parse message JSON %s\n", id);
        goto cleanup;
    }
    const char* snippet=""; const char* subject=NULL; const char* internal=NULL;
    cJSON* sn=cJSON_GetObjectItem(root,"snippet"); if(cJSON_IsString(sn)) snippet=sn->valuestring;
    cJSON* in=cJSON_GetObjectItem(root,"internalDate"); if(cJSON_IsString(in)) internal=in->valuestring;
    cJSON* payload=cJSON_GetObjectItem(root,"payload");
    if(cJSON_IsObject(payload)){
        cJSON* headers=cJSON_GetObjectItem(payload,"headers");
        if(cJSON_IsArray(headers)){
            cJSON* h=NULL; cJSON_ArrayForEach(h,headers){
                cJSON* name=cJSON_GetObjectItem(h,"name");
                cJSON* val=cJSON_GetObjectItem(h,"value");
                if(cJSON_IsString(name)&&cJSON_IsString(val)&&strcmp(name->valuestring,"Subject")==0){
                    subject=val->valuestring; break; }
            }
        }
    }
    uint64_t ts=0; if(internal) ts=strtoull(internal,NULL,10)/1000ULL;
    size_t wlen=mbstowcs(NULL,snippet,0);
    if(wlen!=(size_t)-1){ wcontent=(wchar_t*)malloc((wlen+1)*sizeof(wchar_t)); if(wcontent) mbstowcs(wcontent,snippet,wlen+1); }
    wchar_t wname[MAX_PATH]; if(subject) to_wide(subject,wname,MAX_PATH); else to_wide(id,wname,MAX_PATH);
    wi=(DbWorkItem*)wi_alloc(); if(!wi) goto cleanup;
    wcscpy_s(wi->parent_path,MAX_LONG_PATH,L"gmail");
    wcscpy_s(wi->name,MAX_PATH,wname);
    uint64_t ft=ts? to_filetime(ts):0;
    wi->file_size=0; wi->creation_time=ft; wi->modified_time=ft; wi->access_time=ft;
    wi->attributes=0; wi->stage=INDEX_FULL_CONTENT; wi->op=WI_ADD;
    wi->content=wcontent; wi->preview=NULL; wi->clone_id=0;
    int tries=0; while(!MPMC_Push(g_host.queue,wi)){
        if(WaitForSingleObject(g_host.cancel_event,0)==WAIT_OBJECT_0 || tries++>1000){
            goto cleanup;
        }
        Sleep(0);
    }
    wi=NULL; wcontent=NULL; // ownership passed to queue

cleanup:
    if(root) cJSON_Delete(root);
    if(resp) free(resp);
    if(wi) wi_free(wi);
    if(wcontent) free(wcontent);
}

static BOOL init(const PluginHost* host){
    if(!host) return FALSE; g_host=*host; const char* env=getenv("GMAIL_TOKEN");
    if(env) to_wide(env,g_oauth_token,256); else fprintf(stderr,"[gmail] GMAIL_TOKEN not set\n");
    return TRUE;
}

static void scan(void){
    if(g_oauth_token[0]==L'\0') return; char token_utf8[256];
    to_utf8(g_oauth_token,token_utf8,sizeof(token_utf8));
    char* resp=NULL; cJSON* root=NULL;
    if(!http_get("https://gmail.googleapis.com/gmail/v1/users/me/messages?maxResults=5",token_utf8,&resp)){
        fprintf(stderr,"[gmail] failed to list messages\n");
        goto cleanup;
    }
    root=cJSON_Parse(resp);
    if(!root){
        fprintf(stderr,"[gmail] failed to parse message list JSON\n");
        goto cleanup;
    }
    cJSON* msgs=cJSON_GetObjectItem(root,"messages");
    if(cJSON_IsArray(msgs)){
        cJSON* m=NULL; cJSON_ArrayForEach(m,msgs){
            cJSON* id=cJSON_GetObjectItem(m,"id");
            if(cJSON_IsString(id)) process_message(id->valuestring,token_utf8);
            if(WaitForSingleObject(g_host.cancel_event,0)==WAIT_OBJECT_0) break;
        }
    }

cleanup:
    if(root) cJSON_Delete(root);
    if(resp) free(resp);
}

static void gmail_shutdown(void){
    // no persistent resources
}

static AnythingPlugin g_plugin={
    ANYTHING_PLUGIN_API_VERSION,
    L"Gmail Scanner",
    init,
    scan,
    gmail_shutdown
};

#ifdef _WIN32
__declspec(dllexport)
#endif
AnythingPlugin* Anything_GetPlugin(void){ return &g_plugin; }

