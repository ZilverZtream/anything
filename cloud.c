// cloud.c - basic cloud drive implementations using public APIs
#include "cloud.h"
#include "util.h"
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#pragma comment(lib, "winhttp.lib")

// Simple helper to perform HTTPS requests using WinHTTP. Returns heap-allocated
// response buffer (caller frees via free()).
static BOOL http_request(const wchar_t* host, const wchar_t* path,
                         const wchar_t* method, const wchar_t* headers,
                         const char* body, char** out_buf){
    if(out_buf) *out_buf = NULL;
    HINTERNET hSession = WinHttpOpen(L"AnythingCloud/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if(!hSession) return FALSE;

    HINTERNET hConnect = WinHttpConnect(hSession, host,
                                       INTERNET_DEFAULT_HTTPS_PORT, 0);
    if(!hConnect){ WinHttpCloseHandle(hSession); return FALSE; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, method, path,
                                           NULL, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           WINHTTP_FLAG_SECURE);
    if(!hRequest){
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return FALSE;
    }

    DWORD body_size = body ? (DWORD)strlen(body) : 0;
    BOOL ok = WinHttpSendRequest(hRequest, headers, (DWORD)-1L,
                                 (LPVOID)body, body_size, body_size, 0);
    if(!ok){
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession); return FALSE;
    }
    if(!WinHttpReceiveResponse(hRequest, NULL)){
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession); return FALSE;
    }

    DWORD dwSize = 0, dwDownloaded = 0; char* buffer = NULL; DWORD total = 0;
    do {
        if(!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
        if(dwSize == 0) break;
        char* tmp = (char*)realloc(buffer, total + dwSize + 1);
        if(!tmp){ free(buffer); buffer=NULL; break; }
        buffer = tmp;
        if(!WinHttpReadData(hRequest, buffer + total, dwSize, &dwDownloaded)) break;
        total += dwDownloaded;
    } while(dwSize > 0);

    if(buffer) buffer[total] = '\0';
    if(out_buf) *out_buf = buffer; else free(buffer);

    WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return buffer != NULL;
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
    *out = _strtoui64(buf,NULL,10);
    return TRUE;
}

static uint64_t parse_rfc3339(const char* s){
    int Y,M,D,h,m; float sf;
    if(!s || sscanf(s, "%d-%d-%dT%d:%d:%f", &Y,&M,&D,&h,&m,&sf)!=6) return 0;
    SYSTEMTIME st={0};
    st.wYear=Y; st.wMonth=M; st.wDay=D;
    st.wHour=h; st.wMinute=m; st.wSecond=(WORD)sf;
    FILETIME ft; if(!SystemTimeToFileTime(&st,&ft)) return 0;
    ULARGE_INTEGER uli; uli.LowPart=ft.dwLowDateTime; uli.HighPart=ft.dwHighDateTime;
    return uli.QuadPart;
}

static void enqueue_item(MPMCQueue* q, const wchar_t* parent, const char* name_utf8,
                         uint64_t size, uint64_t ctime, uint64_t mtime, BOOL is_dir){
    if(!q) return;
    DbWorkItem* wi = (DbWorkItem*)_aligned_malloc(sizeof(DbWorkItem), CACHE_LINE_SIZE);
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
    wi->op = WI_ADD;
    while(!MPMC_Push(q, wi)) { SwitchToThread(); }
}

// Obtain OAuth2 token using refresh_token flow. Credentials are taken from environment variables.
static BOOL obtain_token(CloudProvider p, char** out_token){
    if(out_token) *out_token=NULL;
    char client[256]={0}, secret[256]={0}, refresh[512]={0};
    const wchar_t *env_client=NULL, *env_secret=NULL, *env_refresh=NULL;
    const wchar_t *host=NULL, *path=NULL;
    switch(p){
    case CLOUD_ONEDRIVE:
        env_client=L"ONEDRIVE_CLIENT_ID"; env_secret=L"ONEDRIVE_CLIENT_SECRET"; env_refresh=L"ONEDRIVE_REFRESH_TOKEN";
        host=L"login.microsoftonline.com"; path=L"/common/oauth2/v2.0/token"; break;
    case CLOUD_GOOGLE_DRIVE:
        env_client=L"GDRIVE_CLIENT_ID"; env_secret=L"GDRIVE_CLIENT_SECRET"; env_refresh=L"GDRIVE_REFRESH_TOKEN";
        host=L"oauth2.googleapis.com"; path=L"/token"; break;
    case CLOUD_PCLOUD:
        env_client=L"PCLOUD_CLIENT_ID"; env_secret=L"PCLOUD_CLIENT_SECRET"; env_refresh=L"PCLOUD_REFRESH_TOKEN";
        host=L"eapi.pcloud.com"; path=L"/oauth2_token"; break;
    case CLOUD_DROPBOX:
        env_client=L"DROPBOX_CLIENT_ID"; env_secret=L"DROPBOX_CLIENT_SECRET"; env_refresh=L"DROPBOX_REFRESH_TOKEN";
        host=L"api.dropboxapi.com"; path=L"/oauth2/token"; break;
    default: return FALSE;
    }
    DWORD n;
    n = GetEnvironmentVariableA(env_client, client, sizeof(client)); if(n==0 || n>=sizeof(client)) return FALSE;
    n = GetEnvironmentVariableA(env_secret, secret, sizeof(secret)); if(n==0 || n>=sizeof(secret)) return FALSE;
    n = GetEnvironmentVariableA(env_refresh, refresh, sizeof(refresh)); if(n==0 || n>=sizeof(refresh)) return FALSE;
    char body[1024];
    sprintf(body, "client_id=%s&client_secret=%s&refresh_token=%s&grant_type=refresh_token", client, secret, refresh);
    wchar_t headers[128]; wcscpy(headers, L"Content-Type: application/x-www-form-urlencoded\r\n");
    char* resp=NULL;
    if(!http_request(host, path, L"POST", headers, body, &resp)) return FALSE;
    char tok[1024]; if(!json_get_string(resp, resp+strlen(resp), "access_token", tok, sizeof(tok))){ free(resp); return FALSE; }
    *out_token = _strdup(tok);
    free(resp);
    return *out_token!=NULL;
}

// ---- Provider specific scanners ----
static void onedrive_walk(const char* token, const wchar_t* parent, const char* item_id, MPMCQueue* q){
    wchar_t headers[512];
    swprintf(headers,512,L"Authorization: Bearer %S\r\n", token);
    wchar_t path[512];
    if(item_id)
        swprintf(path,512,L"/v1.0/me/drive/items/%S/children?select=id,name,size,folder,fileSystemInfo", item_id);
    else
        wcscpy(path,L"/v1.0/me/drive/root/children?select=id,name,size,folder,fileSystemInfo");
    char* resp=NULL; if(!http_request(L"graph.microsoft.com", path, L"GET", headers, NULL, &resp)) return;
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
            enqueue_item(q,parent,name,size,ct,mt,is_dir);
            if(is_dir){
                wchar_t child_parent[MAX_LONG_PATH];
                wchar_t wname[MAX_PATH]; to_wide(name,wname,MAX_PATH);
                path_join(child_parent,MAX_LONG_PATH,parent,wname);
                onedrive_walk(token,child_parent,id,q);
            }
        }
    }
    free(resp);
}

static void google_drive_walk(const char* token, const wchar_t* parent, const char* folder_id, MPMCQueue* q){
    wchar_t headers[512];
    swprintf(headers,512,L"Authorization: Bearer %S\r\n", token);
    wchar_t path[1024];
    if(folder_id)
        swprintf(path,1024,L"/drive/v3/files?q='%S'+in+parents&fields=files(id,name,mimeType,modifiedTime,size)", folder_id);
    else
        wcscpy(path,L"/drive/v3/files?q='root'+in+parents&fields=files(id,name,mimeType,modifiedTime,size)");
    char* resp=NULL; if(!http_request(L"www.googleapis.com", path, L"GET", headers,NULL,&resp)) return;
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
            enqueue_item(q,parent,name,size,mt,mt,is_dir);
            if(is_dir){
                wchar_t child_parent[MAX_LONG_PATH];
                wchar_t wname[MAX_PATH]; to_wide(name,wname,MAX_PATH);
                path_join(child_parent,MAX_LONG_PATH,parent,wname);
                google_drive_walk(token,child_parent,id,q);
            }
        }
    }
    free(resp);
}

static void pcloud_walk(const char* token, const wchar_t* parent, MPMCQueue* q){
    wchar_t path[512];
    swprintf(path,512,L"/listfolder?auth=%S&folderid=0&recursive=1", token);
    char* resp=NULL; if(!http_request(L"api.pcloud.com", path, L"GET", L"", NULL,&resp)) return;
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
    wchar_t headers[512];
    swprintf(headers,512,L"Authorization: Bearer %S\r\nContent-Type: application/json\r\n", token);
    const char* body="{\"path\":\"\",\"recursive\":true}";
    char* resp=NULL; if(!http_request(L"api.dropboxapi.com", L"/2/files/list_folder", L"POST", headers, body, &resp)) return;
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
    (void)db; // database integration stub for now
    char* token=NULL; if(!obtain_token(provider,&token)) return FALSE;
    wchar_t root[MAX_LONG_PATH]; root[0]=0;
    switch(provider){
    case CLOUD_ONEDRIVE:
        wcscpy(root,L"OneDrive:");
        onedrive_walk(token,root,NULL,out_queue); break;
    case CLOUD_GOOGLE_DRIVE:
        wcscpy(root,L"GDrive:");
        google_drive_walk(token,root,NULL,out_queue); break;
    case CLOUD_PCLOUD:
        wcscpy(root,L"pCloud:");
        pcloud_walk(token,root,out_queue); break;
    case CLOUD_DROPBOX:
        wcscpy(root,L"Dropbox:");
        dropbox_walk(token,root,out_queue); break;
    default:
        free(token); return FALSE;
    }
    free(token);
    return TRUE;
}

