// iCloud Mail Scanner Plugin
// Integrates with Apple's CloudKit Web Services API using OAuth 2.0 tokens obtained via Sign in with Apple.

#include "anything/plugin.h"
#include <wchar.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <curl/curl.h>
#include "../../third_party/cJSON/cJSON.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#define Sleep(ms) usleep((ms)*1000)
static int wcscpy_s(wchar_t* dst,size_t dstcch,const wchar_t* src){
    if(!dst||!src||dstcch==0) return 1; wcsncpy(dst,src,dstcch); dst[dstcch-1]=0; return 0;
}
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

static wchar_t g_oauth_token[4096]=L"";
static wchar_t g_container[256]=L"iCloud.com.apple.mail";
static wchar_t g_environment[64]=L"production";
static PluginHost g_host;

struct curl_buf{ char* data; size_t size; };
static size_t curl_write_cb(void* contents,size_t size,size_t nmemb,void* userp){
    if(size && nmemb > SIZE_MAX / size) return 0;
    size_t realsize=size*nmemb; struct curl_buf* mem=(struct curl_buf*)userp;
    if(realsize > SIZE_MAX - mem->size - 1) return 0;
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

static BOOL http_post(const char* url,const char* token,const char* body,char** out){
    if(out) *out=NULL; static int curl_inited=0;
    if(!curl_inited){ if(curl_global_init(CURL_GLOBAL_DEFAULT)!=0) return FALSE; curl_inited=1; }
    CURL* curl=curl_easy_init(); if(!curl) return FALSE;
    curl_easy_setopt(curl,CURLOPT_URL,url);
    curl_easy_setopt(curl,CURLOPT_POST,1L);
    curl_easy_setopt(curl,CURLOPT_POSTFIELDS,body);
    struct curl_slist* hdr=NULL;
    size_t auth_len = strlen("Authorization: Bearer ") + strlen(token) + 1;
    char* auth = (char*)malloc(auth_len);
    if(!auth){ curl_easy_cleanup(curl); return FALSE; }
    snprintf(auth, auth_len, "Authorization: Bearer %s", token);
    hdr=curl_slist_append(hdr,auth); free(auth);
    hdr=curl_slist_append(hdr,"Content-Type: application/json");
    curl_easy_setopt(curl,CURLOPT_HTTPHEADER,hdr);
    struct curl_buf buf={0};
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,curl_write_cb);
    curl_easy_setopt(curl,CURLOPT_WRITEDATA,&buf);
    CURLcode res=curl_easy_perform(curl);
    if(hdr) curl_slist_free_all(hdr); curl_easy_cleanup(curl);
    if(res!=CURLE_OK){ free(buf.data); return FALSE; }
    if(out) *out=buf.data; else free(buf.data); return TRUE;
}

static uint64_t to_filetime(uint64_t unix_secs){
    return unix_secs*10000000ULL + 116444736000000000ULL;
}

static void process_record(cJSON* rec){
    if(!rec) return; const char* rec_name=NULL; const char* subject=NULL; const char* preview=""; uint64_t ts=0;
    cJSON* rn=cJSON_GetObjectItemCaseSensitive(rec,"recordName"); if(cJSON_IsString(rn)) rec_name=rn->valuestring;
    cJSON* fields=cJSON_GetObjectItemCaseSensitive(rec,"fields");
    if(cJSON_IsObject(fields)){
        cJSON* sub=cJSON_GetObjectItemCaseSensitive(fields,"subject");
        if(cJSON_IsObject(sub)){
            cJSON* val=cJSON_GetObjectItemCaseSensitive(sub,"value"); if(cJSON_IsString(val)) subject=val->valuestring;
        }
        cJSON* prev=cJSON_GetObjectItemCaseSensitive(fields,"preview");
        if(cJSON_IsObject(prev)){
            cJSON* val=cJSON_GetObjectItemCaseSensitive(prev,"value"); if(cJSON_IsString(val)) preview=val->valuestring;
        }
        cJSON* date=cJSON_GetObjectItemCaseSensitive(fields,"sentDate");
        if(cJSON_IsObject(date)){
            cJSON* val=cJSON_GetObjectItemCaseSensitive(date,"value");
            if(cJSON_IsObject(val)){
                cJSON* ts_item=cJSON_GetObjectItemCaseSensitive(val,"timestamp");
                if(cJSON_IsNumber(ts_item)) ts=(uint64_t)ts_item->valuedouble/1000ULL;
            } else if(cJSON_IsNumber(val)) ts=(uint64_t)val->valuedouble/1000ULL;
        }
    }
    wchar_t wname[MAX_PATH]; if(subject) to_wide(subject,wname,MAX_PATH); else if(rec_name) to_wide(rec_name,wname,MAX_PATH); else wcscpy_s(wname,MAX_PATH,L"message");
    wchar_t* wcontent=NULL; size_t wlen=mbstowcs(NULL,preview,0); if(wlen!=(size_t)-1){ wcontent=(wchar_t*)malloc((wlen+1)*sizeof(wchar_t)); if(wcontent) mbstowcs(wcontent,preview,wlen+1); }
    DbWorkItem* wi=(DbWorkItem*)wi_alloc(); if(!wi){ if(wcontent) free(wcontent); return; }
    wcscpy_s(wi->parent_path,MAX_LONG_PATH,L"icloud");
    wcscpy_s(wi->name,MAX_PATH,wname);
    uint64_t ft=ts?to_filetime(ts):0;
    wi->file_size=0; wi->creation_time=ft; wi->modified_time=ft; wi->access_time=ft;
    wi->attributes=0; wi->stage=INDEX_FULL_CONTENT; wi->op=WI_ADD;
    wi->content=wcontent; wi->preview=NULL; wi->clone_id=0;
    wi->hash_crc = 0; wi->hash_ready = FALSE;
    int tries=0; while(!MPMC_Push(g_host.queue,wi)){
        if(is_cancelled(g_host.cancel_token) || tries++>1000){ if(wcontent) free(wcontent); wi_free(wi); return; }
        Sleep(0);
    }
}

static BOOL init(const PluginHost* host){
    if(!host) return FALSE; g_host=*host;

    // WARNING: Reading OAuth tokens from environment variables is INSECURE
    // Environment variables can be read by other processes
    // TODO: Use OS credential manager (Windows: CredRead, macOS: Keychain, Linux: libsecret)
    const char* env=getenv("ICLOUD_TOKEN");
    if(env){
        fprintf(stderr,"[icloud] WARNING: Using ICLOUD_TOKEN from environment variable is insecure!\n");
        fprintf(stderr,"[icloud] Tokens should be stored in OS credential manager.\n");
        to_wide(env,g_oauth_token,4096);
    }

    const char* c=getenv("ICLOUD_CONTAINER"); if(c) to_wide(c,g_container,256);
    const char* e=getenv("ICLOUD_ENV"); if(e) to_wide(e,g_environment,64);
    return TRUE;
}

static void scan(void){
    if(g_oauth_token[0]==L'\0') return; char token_utf8[4096]; to_utf8(g_oauth_token,token_utf8,sizeof(token_utf8));
    char container_utf8[256]; to_utf8(g_container,container_utf8,sizeof(container_utf8));
    char env_utf8[64]; to_utf8(g_environment,env_utf8,sizeof(env_utf8));
    char url[512]; snprintf(url,sizeof(url),"https://api.apple-cloudkit.com/database/1/%s/%s/private/records/query",container_utf8,env_utf8);
    const char* body="{\"query\":{\"recordType\":\"MailMessage\"},\"resultsLimit\":5}";
    char* resp=NULL; cJSON* root=NULL;
    if(!http_post(url,token_utf8,body,&resp)) goto cleanup;
    root=cJSON_Parse(resp);
    if(!root){
        const char* err=cJSON_GetErrorPtr();
        if(err) fprintf(stderr,"iCloud JSON parse error: %s\n",err);
        goto cleanup;
    }
    cJSON* recs=cJSON_GetObjectItemCaseSensitive(root,"records");
    if(cJSON_IsArray(recs)){
        cJSON* r=NULL; cJSON_ArrayForEach(r,recs){ if(is_cancelled(g_host.cancel_token)) break; if(cJSON_IsObject(r)) process_record(r); }
    }

cleanup:
    memset(token_utf8, 0, sizeof(token_utf8));
    if(root) cJSON_Delete(root);
    if(resp) free(resp);
}

static void icloud_shutdown(void){
    memset(g_oauth_token, 0, sizeof(g_oauth_token));
}

static AnythingPlugin g_plugin={
    ANYTHING_PLUGIN_API_VERSION,
    L"iCloud Mail Scanner",
    init,
    scan,
    icloud_shutdown
};

#ifdef _WIN32
__declspec(dllexport)
#endif
AnythingPlugin* Anything_GetPlugin(void){ return &g_plugin; }

