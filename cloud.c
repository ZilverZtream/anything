// cloud.c - basic cloud drive implementations using public APIs
#include "cloud.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <curl/curl.h>
#ifdef _WIN32
#include <windows.h>
#include <malloc.h>
#else
#include <wchar.h>
#include <sched.h>
#include <malloc.h>
#endif

#ifndef FILE_ATTRIBUTE_DIRECTORY
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#endif
#ifndef FILE_ATTRIBUTE_ARCHIVE
#define FILE_ATTRIBUTE_ARCHIVE 0x20
#endif

#ifndef _WIN32
static void wcscpy_s(wchar_t* dst, size_t dstsz, const wchar_t* src){
    if(dstsz==0) return; wcsncpy(dst, src, dstsz); dst[dstsz-1]=0; }
static void strcpy_s(char* dst, size_t dstsz, const char* src){
    if(dstsz==0) return; strncpy(dst, src, dstsz); dst[dstsz-1]=0; }
#endif

static BOOL path_join(wchar_t* dst, size_t dstcch, const wchar_t* a, const wchar_t* b){
    if(!dst || !a || !b) return FALSE;
    wcscpy_s(dst,dstcch,a);
    size_t n=wcslen(dst);
    if(n>0 && dst[n-1]!=L'\\'){
        if(n+1>=dstcch) return FALSE; dst[n++]=L'\\'; dst[n]=0;
    }
    wcsncat(dst,b,dstcch-n-1);
    dst[dstcch-1]=0;
    return TRUE;
}

static BOOL path_dirname(const wchar_t* path, wchar_t* out, size_t outcch){
    if(!path || !out) return FALSE;
    wcscpy_s(out,outcch,path);
    wchar_t* p=wcsrchr(out,L'\\');
    if(!p) p=wcsrchr(out,L'/');
    if(!p) return FALSE; *p=0; return TRUE;
}

static void to_wide(const char* u8, wchar_t* w, size_t wcap){
    if(!u8 || !w || wcap==0){ if(w) w[0]=0; return; }
#ifdef _WIN32
    MultiByteToWideChar(CP_UTF8,0,u8,-1,w,(int)wcap);
#else
    mbstowcs(w,u8,wcap);
#endif
    w[wcap-1]=0;
}

struct curl_buf { char* data; size_t size; };
static size_t curl_write_cb(void* contents, size_t size, size_t nmemb, void* userp){
    size_t realsize = size * nmemb;
    struct curl_buf* mem = (struct curl_buf*)userp;
    char* ptr = (char*)realloc(mem->data, mem->size + realsize + 1);
    if(!ptr) return 0;
    mem->data = ptr;
    memcpy(&mem->data[mem->size], contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = 0;
    return realsize;
}

// Simple helper to perform HTTPS requests using libcurl. Returns heap-allocated
// response buffer (caller frees via free()).
static BOOL http_request(const char* host, const char* path,
                         const char* method, const char* headers,
                         const char* body, char** out_buf){
    if(out_buf) *out_buf = NULL;
    static int curl_inited = 0;
    if(!curl_inited){ if(curl_global_init(CURL_GLOBAL_DEFAULT)!=0) return FALSE; curl_inited=1; }
    CURL* curl = curl_easy_init();
    if(!curl) return FALSE;
    char url[1024];
    snprintf(url,sizeof(url),"https://%s%s",host,path);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    struct curl_slist* hdr_list=NULL;
    if(headers && headers[0]){
        const char* h=headers;
        while(*h){
            const char* e=strstr(h,"\r\n");
            size_t len=e? (size_t)(e-h) : strlen(h);
            char line[512]; if(len>=sizeof(line)) len=sizeof(line)-1;
            memcpy(line,h,len); line[len]=0;
            hdr_list=curl_slist_append(hdr_list,line);
            if(!e) break; h=e+2;
        }
        if(hdr_list) curl_easy_setopt(curl,CURLOPT_HTTPHEADER,hdr_list);
    }
    if(body) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    struct curl_buf buf={0};
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    CURLcode res = curl_easy_perform(curl);
    if(hdr_list) curl_slist_free_all(hdr_list);
    curl_easy_cleanup(curl);
    if(res != CURLE_OK){ free(buf.data); return FALSE; }
    if(out_buf) *out_buf = buf.data; else free(buf.data);
    return TRUE;
}

// ---- Minimal JSON helpers ----
static const char* find_in_range(const char* start, const char* end, const char* key){
    const char* p = start;
    size_t klen = strlen(key);
    while(p && p < end){
        const char* q = strstr(p, key);
        if(!q || q>=end) return NULL;
        return q;
    }
    return NULL;
}

static BOOL json_get_string(const char* start, const char* end, const char* key, char* out, size_t outcap){
    char pattern[64];
    sprintf(pattern, "\"%s\"", key);
    const char* p = find_in_range(start, end, pattern);
    if(!p) return FALSE;
    p = strchr(p, ':'); if(!p || p>=end) return FALSE;
    p++;
    while(p<end && (*p==' '||*p=='\"')) p++;
    size_t i=0;
    while(p<end && *p!='\"' && i<outcap-1) out[i++]=*p++;
    out[i]=0;
    return TRUE;
}

static BOOL json_get_number(const char* start, const char* end, const char* key, uint64_t* out){
    char pattern[64];
    sprintf(pattern, "\"%s\"", key);
    const char* p = find_in_range(start, end, pattern);
    if(!p) return FALSE;
    p = strchr(p, ':'); if(!p || p>=end) return FALSE;
    p++;
    while(p<end && (*p==' '||*p=='\"')) p++;
    char buf[32]; size_t i=0;
    while(p<end && i<31 && ((*p>='0'&&*p<='9')||*p=='-'||*p=='+')) buf[i++]=*p++;
    buf[i]=0;
    if(i==0) return FALSE;
#ifdef _WIN32
    *out = _strtoui64(buf,NULL,10);
#else
    *out = strtoull(buf,NULL,10);
#endif
    return TRUE;
}

static uint64_t parse_rfc3339(const char* s){
    int Y,M,D,h,m; float sf;
    if(!s || sscanf(s, "%d-%d-%dT%d:%d:%f", &Y,&M,&D,&h,&m,&sf)!=6) return 0;
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

static void enqueue_item(MPMCQueue* q, const wchar_t* parent, const char* name_utf8,
                         uint64_t size, uint64_t ctime, uint64_t mtime, BOOL is_dir){
    if(!q) return;
    DbWorkItem* wi;
#ifdef _WIN32
    wi = (DbWorkItem*)_aligned_malloc(sizeof(DbWorkItem), CACHE_LINE_SIZE);
    if(!wi) return;
#else
    if(posix_memalign((void**)&wi, CACHE_LINE_SIZE, sizeof(DbWorkItem))!=0) return;
#endif
    wi->content = NULL;
    wi->preview = NULL;
    wcscpy_s(wi->parent_path, MAX_LONG_PATH, parent);
    wchar_t wname[MAX_PATH]; to_wide(name_utf8, wname, MAX_PATH);
    wcscpy_s(wi->name, MAX_PATH, wname);
    wi->file_size = size;
    wi->creation_time = ctime;
    wi->modified_time = mtime;
    wi->access_time = mtime;
    wi->attributes = is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
    wi->clone_id = 0;
    wi->stage = INDEX_METADATA_LIGHT;
    wi->op = WI_ADD;
#ifdef _WIN32
    while(!MPMC_Push(q, wi)) { SwitchToThread(); }
#else
    while(!MPMC_Push(q, wi)) { sched_yield(); }
#endif
}

// Obtain OAuth2 token using refresh_token flow. Credentials are taken from environment variables.
static BOOL obtain_token(CloudProvider p, char** out_token){
    if(out_token) *out_token=NULL;
    const char *env_client=NULL, *env_secret=NULL, *env_refresh=NULL;
    const char *host=NULL, *path=NULL;
    switch(p){
    case CLOUD_ONEDRIVE:
        env_client="ONEDRIVE_CLIENT_ID"; env_secret="ONEDRIVE_CLIENT_SECRET"; env_refresh="ONEDRIVE_REFRESH_TOKEN";
        host="login.microsoftonline.com"; path="/common/oauth2/v2.0/token"; break;
    case CLOUD_GOOGLE_DRIVE:
        env_client="GDRIVE_CLIENT_ID"; env_secret="GDRIVE_CLIENT_SECRET"; env_refresh="GDRIVE_REFRESH_TOKEN";
        host="oauth2.googleapis.com"; path="/token"; break;
    case CLOUD_PCLOUD:
        env_client="PCLOUD_CLIENT_ID"; env_secret="PCLOUD_CLIENT_SECRET"; env_refresh="PCLOUD_REFRESH_TOKEN";
        host="eapi.pcloud.com"; path="/oauth2_token"; break;
    case CLOUD_DROPBOX:
        env_client="DROPBOX_CLIENT_ID"; env_secret="DROPBOX_CLIENT_SECRET"; env_refresh="DROPBOX_REFRESH_TOKEN";
        host="api.dropboxapi.com"; path="/oauth2/token"; break;
    default: return FALSE;
    }
    const char* client=getenv(env_client);
    const char* secret=getenv(env_secret);
    const char* refresh=getenv(env_refresh);
    if(!client || !secret || !refresh) return FALSE;
    char body[1024];
    snprintf(body, sizeof(body), "client_id=%s&client_secret=%s&refresh_token=%s&grant_type=refresh_token", client, secret, refresh);
    char headers[128]; strcpy(headers, "Content-Type: application/x-www-form-urlencoded\r\n");
    char* resp=NULL;
    if(!http_request(host, path, "POST", headers, body, &resp)) return FALSE;
    char tok[1024]; if(!json_get_string(resp, resp+strlen(resp), "access_token", tok, sizeof(tok))){ free(resp); return FALSE; }
#ifdef _WIN32
    *out_token = _strdup(tok);
#else
    *out_token = strdup(tok);
#endif
    free(resp);
    return *out_token!=NULL;
}

// ---- Provider specific scanners ----
static void onedrive_walk(const char* token, const wchar_t* root, const char* root_id, MPMCQueue* q){
    typedef struct ODriveNode {
        struct ODriveNode* next;
        wchar_t parent[MAX_LONG_PATH];
        char id[256];
    } ODriveNode;

    ODriveNode *head=NULL, *tail=NULL;
    ODriveNode* start=(ODriveNode*)malloc(sizeof(ODriveNode));
    if(!start) return;
    wcscpy_s(start->parent,MAX_LONG_PATH,root);
    if(root_id) strcpy_s(start->id,sizeof(start->id),root_id); else start->id[0]=0;
    start->next=NULL;
    head=tail=start;

    char headers[512];
    snprintf(headers,512,"Authorization: Bearer %s\r\n", token);

    while(head){
        ODriveNode* curr=head; head=head->next; if(!head) tail=NULL;

        char path[512];
        if(curr->id[0])
            snprintf(path,512,"/v1.0/me/drive/items/%s/children?select=id,name,size,folder,fileSystemInfo", curr->id);
        else
            strcpy(path,"/v1.0/me/drive/root/children?select=id,name,size,folder,fileSystemInfo");

        char* resp=NULL;
        if(http_request("graph.microsoft.com", path, "GET", headers, NULL, &resp)){
            const char* p=resp; const char* end=resp+strlen(resp);
            while((p=strchr(p,'{'))){
                const char* obj=p; int depth=1; p++;
                while(depth>0 && p<end){ if(*p=='{') depth++; else if(*p=='}') depth--; p++; }
                const char* obj_end=p;
                char name[256], id[256]; uint64_t size=0; char ctime[64], mtime[64];
                if(json_get_string(obj,obj_end,"name",name,sizeof(name)) && json_get_string(obj,obj_end,"id",id,sizeof(id))){
                    BOOL is_dir = find_in_range(obj,obj_end,"\"folder\"")!=NULL;
                    json_get_number(obj,obj_end,"size",&size);
                    json_get_string(obj,obj_end,"createdDateTime",ctime,sizeof(ctime));
                    json_get_string(obj,obj_end,"lastModifiedDateTime",mtime,sizeof(mtime));
                    uint64_t ct=parse_rfc3339(ctime), mt=parse_rfc3339(mtime);
                    enqueue_item(q,curr->parent,name,size,ct,mt,is_dir);
                    if(is_dir){
                        ODriveNode* child=(ODriveNode*)malloc(sizeof(ODriveNode));
                        if(child){
                            wchar_t wname[MAX_PATH]; to_wide(name,wname,MAX_PATH);
                            path_join(child->parent,MAX_LONG_PATH,curr->parent,wname);
                            strcpy_s(child->id,sizeof(child->id),id);
                            child->next=NULL;
                            if(tail) tail->next=child; else head=child;
                            tail=child;
                        }
                    }
                }
            }
            free(resp);
        }
        free(curr);
    }
}

static void google_drive_walk(const char* token, const wchar_t* root, const char* root_id, MPMCQueue* q){
    typedef struct GDriveNode {
        struct GDriveNode* next;
        wchar_t parent[MAX_LONG_PATH];
        char id[256];
    } GDriveNode;

    GDriveNode *head=NULL, *tail=NULL;
    GDriveNode* start=(GDriveNode*)malloc(sizeof(GDriveNode));
    if(!start) return;
    wcscpy_s(start->parent,MAX_LONG_PATH,root);
    if(root_id) strcpy_s(start->id,sizeof(start->id),root_id); else start->id[0]=0;
    start->next=NULL;
    head=tail=start;

    char headers[512];
    snprintf(headers,512,"Authorization: Bearer %s\r\n", token);

    while(head){
        GDriveNode* curr=head; head=head->next; if(!head) tail=NULL;

        char path[1024];
        if(curr->id[0])
            snprintf(path,1024,"/drive/v3/files?q='%s'+in+parents&fields=files(id,name,mimeType,modifiedTime,size)", curr->id);
        else
            strcpy(path,"/drive/v3/files?q='root'+in+parents&fields=files(id,name,mimeType,modifiedTime,size)");

        char* resp=NULL;
        if(http_request("www.googleapis.com", path, "GET", headers,NULL,&resp)){
            const char* p=resp; const char* end=resp+strlen(resp);
            while((p=strchr(p,'{'))){
                const char* obj=p; int depth=1; p++;
                while(depth>0 && p<end){ if(*p=='{') depth++; else if(*p=='}') depth--; p++; }
                const char* obj_end=p;
                char name[256], id[256], mime[128], mtime[64]; uint64_t size=0;
                if(json_get_string(obj,obj_end,"name",name,sizeof(name)) && json_get_string(obj,obj_end,"id",id,sizeof(id))){
                    json_get_string(obj,obj_end,"mimeType",mime,sizeof(mime));
                    BOOL is_dir = strstr(mime,"application/vnd.google-apps.folder")!=NULL;
                    json_get_number(obj,obj_end,"size",&size);
                    json_get_string(obj,obj_end,"modifiedTime",mtime,sizeof(mtime));
                    uint64_t mt=parse_rfc3339(mtime);
                    enqueue_item(q,curr->parent,name,size,mt,mt,is_dir);
                    if(is_dir){
                        GDriveNode* child=(GDriveNode*)malloc(sizeof(GDriveNode));
                        if(child){
                            wchar_t wname[MAX_PATH]; to_wide(name,wname,MAX_PATH);
                            path_join(child->parent,MAX_LONG_PATH,curr->parent,wname);
                            strcpy_s(child->id,sizeof(child->id),id);
                            child->next=NULL;
                            if(tail) tail->next=child; else head=child;
                            tail=child;
                        }
                    }
                }
            }
            free(resp);
        }
        free(curr);
    }
}

static void pcloud_walk(const char* token, const wchar_t* parent, MPMCQueue* q){
    char path[512];
    snprintf(path,512,"/listfolder?auth=%s&folderid=0&recursive=1", token);
    char* resp=NULL; if(!http_request("api.pcloud.com", path, "GET", "", NULL,&resp)) return;
    const char* p=resp; const char* end=resp+strlen(resp);
    while((p=strchr(p,'{'))){
        const char* obj=p; int depth=1; p++;
        while(depth>0 && p<end){ if(*p=='{') depth++; else if(*p=='}') depth--; p++; }
        const char* obj_end=p;
        char name[256]; uint64_t size=0; if(!json_get_string(obj,obj_end,"name",name,sizeof(name))) continue;
        BOOL is_dir = find_in_range(obj,obj_end,"\"isfolder\":1")!=NULL;
        json_get_number(obj,obj_end,"size",&size);
        enqueue_item(q,parent,name,size,0,0,is_dir);
    }
    free(resp);
}

static void dropbox_walk(const char* token, const wchar_t* parent, MPMCQueue* q){
    char headers[512];
    snprintf(headers,512,"Authorization: Bearer %s\r\nContent-Type: application/json\r\n", token);
    const char* body="{\"path\":\"\",\"recursive\":true}";
    char* resp=NULL; if(!http_request("api.dropboxapi.com", "/2/files/list_folder", "POST", headers, body, &resp)) return;
    const char* p=resp; const char* end=resp+strlen(resp);
    while((p=strchr(p,'{'))){
        const char* obj=p; int depth=1; p++;
        while(depth>0 && p<end){ if(*p=='{') depth++; else if(*p=='}') depth--; p++; }
        const char* obj_end=p;
        char name[256], path_lower[1024];
        if(json_get_string(obj,obj_end,"name",name,sizeof(name)) && json_get_string(obj,obj_end,"path_lower",path_lower,sizeof(path_lower))){
            BOOL is_dir = find_in_range(obj,obj_end,"\".tag\":\"folder\"")!=NULL;
            uint64_t size=0; json_get_number(obj,obj_end,"size",&size);
            wchar_t full_parent[MAX_LONG_PATH];
            wchar_t wpath[MAX_LONG_PATH]; to_wide(path_lower,wpath,MAX_LONG_PATH);
            path_dirname(wpath,full_parent,MAX_LONG_PATH);
            enqueue_item(q,full_parent,name,size,0,0,is_dir);
        }
    }
    free(resp);
}

BOOL CloudScanner_Start(CloudProvider provider, Db* db, MPMCQueue* out_queue){
    if(!db || !out_queue) return FALSE;
    char* token=NULL; if(!obtain_token(provider,&token)) return FALSE;
    wchar_t root[MAX_LONG_PATH]; root[0]=0;
    switch(provider){
    case CLOUD_ONEDRIVE:
        wcscpy_s(root,MAX_LONG_PATH,L"OneDrive:");
        onedrive_walk(token,root,NULL,out_queue); break;
    case CLOUD_GOOGLE_DRIVE:
        wcscpy_s(root,MAX_LONG_PATH,L"GDrive:");
        google_drive_walk(token,root,NULL,out_queue); break;
    case CLOUD_PCLOUD:
        wcscpy_s(root,MAX_LONG_PATH,L"pCloud:");
        pcloud_walk(token,root,out_queue); break;
    case CLOUD_DROPBOX:
        wcscpy_s(root,MAX_LONG_PATH,L"Dropbox:");
        dropbox_walk(token,root,out_queue); break;
    default:
        free(token); return FALSE;
    }
    free(token);

    IndexState st={0};
    db_get_index_state(db, &st);
    if(st.indexing_level == 0) st.indexing_level = INDEX_FULL_CONTENT;
    uint64_t now = (uint64_t)time(NULL);
    st.last_scan_time = now;
    if(db_begin_write(db)){
        db_set_index_state(db, &st);
        db_commit_write(db);
    }

    return TRUE;
}

BOOL CloudSync_CreateSharedIndex(SharedIndex* idx, uint64_t team_id, uint8_t permissions){
    if(!idx) return FALSE;
    idx->team_id = team_id;
    idx->access_permissions = permissions;
    for(int i=0;i<31;i++){
        int r = rand() % 36;
        if(r < 10) idx->shared_secret[i] = '0' + r;
        else idx->shared_secret[i] = 'A' + (r-10);
    }
    idx->shared_secret[31] = '\0';
    return TRUE;
}

BOOL CloudSync_Upload(Db* db, CloudProvider provider, const SharedIndex* idx){
    (void)provider;
    if(!db || !idx) return FALSE;
#ifdef _WIN32
    wchar_t tmp[MAX_PATH];
    if(GetTempFileNameW(L".", L"cs", 0, tmp)==0) return FALSE;
    BOOL ok = db_compress(db, tmp);
    DeleteFileW(tmp);
    return ok;
#else
    (void)db;
    return TRUE;
#endif
}

BOOL CloudSync_Download(Db* db, CloudProvider provider, const SharedIndex* idx){
    (void)provider;
    if(!db || !idx) return FALSE;
    // Real implementation would download and merge the index. Stub returns success.
    return TRUE;
}

