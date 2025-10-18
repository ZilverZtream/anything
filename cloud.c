// cloud.c - basic cloud drive implementations using public APIs
#include "cloud.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <curl/curl.h>
#include "cJSON.h"
#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#include <malloc.h>
#else
#include <wchar.h>
#include <sched.h>
#include <malloc.h>
#include <sys/mman.h>
#include <unistd.h>
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

static void secure_memzero(void* ptr, size_t len){
    if(!ptr || len==0) return;
#ifdef _WIN32
    SecureZeroMemory(ptr, len);
#else
    volatile unsigned char* p = (volatile unsigned char*)ptr;
    while(len--) *p++ = 0;
#endif
}

typedef struct SensitiveBuffer {
    void* data;
    size_t size;
    size_t alloc_size;
} SensitiveBuffer;

static BOOL sensitive_alloc(SensitiveBuffer* buf, size_t size){
    if(!buf || size==0) return FALSE;
    buf->data = NULL;
    buf->size = size;
    buf->alloc_size = 0;
#ifdef _WIN32
    SYSTEM_INFO sys_info;
    GetSystemInfo(&sys_info);
    size_t page = sys_info.dwPageSize ? (size_t)sys_info.dwPageSize : 4096;
    size_t alloc_size = ((size + page - 1) / page) * page;
    void* mem = VirtualAlloc(NULL, alloc_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if(!mem) return FALSE;
    if(!VirtualLock(mem, alloc_size)){
        VirtualFree(mem, 0, MEM_RELEASE);
        return FALSE;
    }
    buf->data = mem;
    buf->alloc_size = alloc_size;
    return TRUE;
#else
    long page = sysconf(_SC_PAGESIZE);
    if(page <= 0) page = 4096;
    size_t pg = (size_t)page;
    size_t alloc_size = ((size + pg - 1) / pg) * pg;
    void* mem = NULL;
    if(posix_memalign(&mem, pg, alloc_size)!=0) return FALSE;
    if(mlock(mem, alloc_size)!=0){
        free(mem);
        return FALSE;
    }
    buf->data = mem;
    buf->alloc_size = alloc_size;
    return TRUE;
#endif
}

static void sensitive_free(SensitiveBuffer* buf){
    if(!buf || !buf->data) return;
    secure_memzero(buf->data, buf->alloc_size);
#ifdef _WIN32
    VirtualUnlock(buf->data, buf->alloc_size);
    VirtualFree(buf->data, 0, MEM_RELEASE);
#else
    munlock(buf->data, buf->alloc_size);
    free(buf->data);
#endif
    buf->data = NULL;
    buf->size = 0;
    buf->alloc_size = 0;
}

#ifdef _WIN32
typedef struct CloudToken {
    DATA_BLOB encrypted;
} CloudToken;
#else
typedef struct CloudToken {
    SensitiveBuffer buffer;
} CloudToken;
#endif

static void cloud_token_init(CloudToken* token){
    if(!token) return;
#ifdef _WIN32
    token->encrypted.cbData = 0;
    token->encrypted.pbData = NULL;
#else
    token->buffer.data = NULL;
    token->buffer.size = 0;
    token->buffer.alloc_size = 0;
#endif
}

static void cloud_token_clear(CloudToken* token){
    if(!token) return;
#ifdef _WIN32
    if(token->encrypted.pbData){
        secure_memzero(token->encrypted.pbData, token->encrypted.cbData);
        LocalFree(token->encrypted.pbData);
        token->encrypted.pbData = NULL;
        token->encrypted.cbData = 0;
    }
#else
    sensitive_free(&token->buffer);
#endif
}

#ifdef _WIN32
static BOOL cloud_token_decrypt(const CloudToken* token, SensitiveBuffer* out_plain){
    if(!token || !out_plain || !token->encrypted.pbData || token->encrypted.cbData==0) return FALSE;
    DATA_BLOB plain = {0};
    if(!CryptUnprotectData((DATA_BLOB*)&token->encrypted, NULL, NULL, NULL, NULL, 0, &plain))
        return FALSE;
    BOOL ok = FALSE;
    if(sensitive_alloc(out_plain, plain.cbData)){
        memcpy(out_plain->data, plain.pbData, plain.cbData);
        ok = TRUE;
    }
    if(plain.pbData){
        secure_memzero(plain.pbData, plain.cbData);
        LocalFree(plain.pbData);
    }
    if(!ok) sensitive_free(out_plain);
    return ok;
}
#else
static BOOL cloud_token_decrypt(const CloudToken* token, SensitiveBuffer* out_plain){
    if(!token || !out_plain || !token->buffer.data) return FALSE;
    if(!sensitive_alloc(out_plain, token->buffer.size)) return FALSE;
    memcpy(out_plain->data, token->buffer.data, token->buffer.size);
    return TRUE;
}
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
    if(!ptr){
        free(mem->data);
        mem->data = NULL;
        mem->size = 0;
        return 0;
    }
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
    DbWorkItem* wi = (DbWorkItem*)aligned_malloc(sizeof(DbWorkItem), CACHE_LINE_SIZE);
    if(!wi) return;
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
static BOOL obtain_token(CloudProvider p, CloudToken* out_token){
    if(!out_token) return FALSE;
    cloud_token_init(out_token);
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
    cJSON* root=NULL;
    BOOL ok=FALSE;
    if(!http_request(host, path, "POST", headers, body, &resp)) goto cleanup;
    root = cJSON_Parse(resp);
    if(!root) goto cleanup;
    cJSON* tok = cJSON_GetObjectItemCaseSensitive(root, "access_token");
    if(!cJSON_IsString(tok) || !tok->valuestring) goto cleanup;
#ifdef _WIN32
    size_t token_len = strlen(tok->valuestring) + 1;
    SensitiveBuffer plain = {0};
    if(!sensitive_alloc(&plain, token_len)) goto cleanup;
    memcpy(plain.data, tok->valuestring, token_len);
    DATA_BLOB in = {0};
    in.cbData = (DWORD)token_len;
    in.pbData = (BYTE*)plain.data;
    DATA_BLOB out = {0};
    if(!CryptProtectData(&in, L"CloudAccessToken", NULL, NULL, NULL, 0, &out)){
        sensitive_free(&plain);
        goto cleanup;
    }
    secure_memzero(plain.data, plain.alloc_size);
    sensitive_free(&plain);
    out_token->encrypted = out;
    ok = TRUE;
#else
    size_t token_len = strlen(tok->valuestring) + 1;
    if(!sensitive_alloc(&out_token->buffer, token_len)) goto cleanup;
    memcpy(out_token->buffer.data, tok->valuestring, token_len);
    ok = TRUE;
#endif

cleanup:
    if(root) cJSON_Delete(root);
    if(resp){
        secure_memzero(resp, strlen(resp));
        free(resp);
    }
    secure_memzero(body, sizeof(body));
    secure_memzero(headers, sizeof(headers));
    return ok;
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
        if(!http_request("graph.microsoft.com", path, "GET", headers, NULL, &resp)){
            free(curr);
            goto cleanup_odrive;
        }
        cJSON* root = cJSON_Parse(resp);
        if(!root){
            free(resp);
            free(curr);
            goto cleanup_odrive;
        }
        cJSON* value = cJSON_GetObjectItemCaseSensitive(root, "value");
        if(!cJSON_IsArray(value)){
            cJSON_Delete(root);
            free(resp);
            free(curr);
            goto cleanup_odrive;
        }
        cJSON* item;
        cJSON_ArrayForEach(item, value){
            cJSON* name = cJSON_GetObjectItemCaseSensitive(item, "name");
            cJSON* id = cJSON_GetObjectItemCaseSensitive(item, "id");
            if(cJSON_IsString(name) && cJSON_IsString(id)){
                uint64_t size=0; uint64_t ct=0, mt=0;
                cJSON* sizeItem = cJSON_GetObjectItemCaseSensitive(item, "size");
                if(cJSON_IsNumber(sizeItem)) size = (uint64_t)sizeItem->valuedouble;
                cJSON* fsi = cJSON_GetObjectItemCaseSensitive(item, "fileSystemInfo");
                if(cJSON_IsObject(fsi)){
                    cJSON* ctime = cJSON_GetObjectItemCaseSensitive(fsi, "createdDateTime");
                    cJSON* mtime = cJSON_GetObjectItemCaseSensitive(fsi, "lastModifiedDateTime");
                    if(cJSON_IsString(ctime)) ct = parse_rfc3339(ctime->valuestring);
                    if(cJSON_IsString(mtime)) mt = parse_rfc3339(mtime->valuestring);
                }
                BOOL is_dir = cJSON_GetObjectItemCaseSensitive(item, "folder") != NULL;
                enqueue_item(q, curr->parent, name->valuestring, size, ct, mt, is_dir);
                if(is_dir){
                    ODriveNode* child=(ODriveNode*)malloc(sizeof(ODriveNode));
                    if(child){
                        wchar_t wname[MAX_PATH]; to_wide(name->valuestring,wname,MAX_PATH);
                        path_join(child->parent,MAX_LONG_PATH,curr->parent,wname);
                        strcpy_s(child->id,sizeof(child->id),id->valuestring);
                        child->next=NULL;
                        if(tail) tail->next=child; else head=child;
                        tail=child;
                    }
                }
            }
        }
        cJSON_Delete(root);
        free(resp);
        free(curr);
    }

cleanup_odrive:
    while(head){
        ODriveNode* tmp=head;
        head=head->next;
        free(tmp);
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
        if(!http_request("www.googleapis.com", path, "GET", headers,NULL,&resp)){
            free(curr);
            goto cleanup_gdrive;
        }
        cJSON* root = cJSON_Parse(resp);
        if(!root){
            free(resp);
            free(curr);
            goto cleanup_gdrive;
        }
        cJSON* files = cJSON_GetObjectItemCaseSensitive(root, "files");
        if(!cJSON_IsArray(files)){
            cJSON_Delete(root);
            free(resp);
            free(curr);
            goto cleanup_gdrive;
        }
        cJSON* item;
        cJSON_ArrayForEach(item, files){
            cJSON* name = cJSON_GetObjectItemCaseSensitive(item, "name");
            cJSON* id = cJSON_GetObjectItemCaseSensitive(item, "id");
            if(cJSON_IsString(name) && cJSON_IsString(id)){
                const char* mime = NULL;
                cJSON* mimeItem = cJSON_GetObjectItemCaseSensitive(item, "mimeType");
                if(cJSON_IsString(mimeItem)) mime = mimeItem->valuestring;
                BOOL is_dir = (mime && strstr(mime, "application/vnd.google-apps.folder")!=NULL);
                uint64_t size=0;
                cJSON* sizeItem = cJSON_GetObjectItemCaseSensitive(item, "size");
                if(cJSON_IsString(sizeItem) && sizeItem->valuestring)
                    size = strtoull(sizeItem->valuestring, NULL, 10);
                else if(cJSON_IsNumber(sizeItem))
                    size = (uint64_t)sizeItem->valuedouble;
                uint64_t mt=0;
                cJSON* mtime = cJSON_GetObjectItemCaseSensitive(item, "modifiedTime");
                if(cJSON_IsString(mtime)) mt = parse_rfc3339(mtime->valuestring);
                enqueue_item(q,curr->parent,name->valuestring,size,mt,mt,is_dir);
                if(is_dir){
                    GDriveNode* child=(GDriveNode*)malloc(sizeof(GDriveNode));
                    if(child){
                        wchar_t wname[MAX_PATH]; to_wide(name->valuestring,wname,MAX_PATH);
                        path_join(child->parent,MAX_LONG_PATH,curr->parent,wname);
                        strcpy_s(child->id,sizeof(child->id),id->valuestring);
                        child->next=NULL;
                        if(tail) tail->next=child; else head=child;
                        tail=child;
                    }
                }
            }
        }
        cJSON_Delete(root);
        free(resp);
        free(curr);
    }

cleanup_gdrive:
    while(head){
        GDriveNode* tmp=head;
        head=head->next;
        free(tmp);
    }
}

static void pcloud_process(cJSON* contents, const wchar_t* parent, MPMCQueue* q){
    if(!cJSON_IsArray(contents)) return;
    cJSON* item;
    cJSON_ArrayForEach(item, contents){
        cJSON* name = cJSON_GetObjectItemCaseSensitive(item, "name");
        if(!cJSON_IsString(name)) continue;
        cJSON* sizeItem = cJSON_GetObjectItemCaseSensitive(item, "size");
        cJSON* isfolder = cJSON_GetObjectItemCaseSensitive(item, "isfolder");
        uint64_t size=0;
        if(cJSON_IsNumber(sizeItem)) size = (uint64_t)sizeItem->valuedouble;
        BOOL is_dir = cJSON_IsNumber(isfolder) && isfolder->valueint==1;
        enqueue_item(q,parent,name->valuestring,size,0,0,is_dir);
        if(is_dir){
            wchar_t child_parent[MAX_LONG_PATH];
            wchar_t wname[MAX_PATH]; to_wide(name->valuestring,wname,MAX_PATH);
            path_join(child_parent,MAX_LONG_PATH,parent,wname);
            cJSON* sub = cJSON_GetObjectItemCaseSensitive(item, "contents");
            if(sub) pcloud_process(sub, child_parent, q);
        }
    }
}

static void pcloud_walk(const char* token, const wchar_t* parent, MPMCQueue* q){
    char path[512];
    snprintf(path,512,"/listfolder?auth=%s&folderid=0&recursive=1", token);
    char* resp=NULL; if(!http_request("api.pcloud.com", path, "GET", "", NULL,&resp)) return;
    cJSON* root = cJSON_Parse(resp);
    if(root){
        cJSON* metadata = cJSON_GetObjectItemCaseSensitive(root, "metadata");
        if(metadata){
            cJSON* contents = cJSON_GetObjectItemCaseSensitive(metadata, "contents");
            if(contents) pcloud_process(contents, parent, q);
        }
        cJSON_Delete(root);
    }
    free(resp);
}

static void dropbox_walk(const char* token, const wchar_t* parent, MPMCQueue* q){
    char headers[512];
    snprintf(headers,512,"Authorization: Bearer %s\r\nContent-Type: application/json\r\n", token);
    const char* body="{\"path\":\"\",\"recursive\":true}";
    char* resp=NULL; if(!http_request("api.dropboxapi.com", "/2/files/list_folder", "POST", headers, body, &resp)) return;
    cJSON* root = cJSON_Parse(resp);
    if(root){
        cJSON* entries = cJSON_GetObjectItemCaseSensitive(root, "entries");
        if(cJSON_IsArray(entries)){
            cJSON* item;
            cJSON_ArrayForEach(item, entries){
                cJSON* name = cJSON_GetObjectItemCaseSensitive(item, "name");
                cJSON* path_lower = cJSON_GetObjectItemCaseSensitive(item, "path_lower");
                if(cJSON_IsString(name) && cJSON_IsString(path_lower)){
                    cJSON* tag = cJSON_GetObjectItemCaseSensitive(item, ".tag");
                    BOOL is_dir = cJSON_IsString(tag) && strcmp(tag->valuestring,"folder")==0;
                    uint64_t size=0;
                    cJSON* sizeItem = cJSON_GetObjectItemCaseSensitive(item, "size");
                    if(cJSON_IsNumber(sizeItem)) size = (uint64_t)sizeItem->valuedouble;
                    wchar_t full_parent[MAX_LONG_PATH];
                    wchar_t wpath[MAX_LONG_PATH]; to_wide(path_lower->valuestring,wpath,MAX_LONG_PATH);
                    path_dirname(wpath,full_parent,MAX_LONG_PATH);
                    enqueue_item(q,full_parent,name->valuestring,size,0,0,is_dir);
                }
            }
        }
        cJSON_Delete(root);
    }
    free(resp);
}

BOOL CloudScanner_Start(CloudProvider provider, Db* db, MPMCQueue* out_queue){
    if(!db || !out_queue) return FALSE;
    CloudToken token_store; cloud_token_init(&token_store);
    if(!obtain_token(provider,&token_store)){
        cloud_token_clear(&token_store);
        return FALSE;
    }
    SensitiveBuffer plain_token={0};
    if(!cloud_token_decrypt(&token_store,&plain_token)){
        cloud_token_clear(&token_store);
        return FALSE;
    }
    if(plain_token.size == 0 || !plain_token.data){
        sensitive_free(&plain_token);
        cloud_token_clear(&token_store);
        return FALSE;
    }
    ((char*)plain_token.data)[plain_token.size-1] = '\0';
    char* token = (char*)plain_token.data;
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
        sensitive_free(&plain_token);
        cloud_token_clear(&token_store);
        return FALSE;
    }
    sensitive_free(&plain_token);
    cloud_token_clear(&token_store);

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

