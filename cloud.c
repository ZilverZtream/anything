// cloud.c - basic cloud drive implementations using public APIs
#include "cloud.h"
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
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

// Very small JSON helper: searches for "name" fields and prints them.
static void process_json_listing(const char* json){
    const char* p = json;
    while((p = strstr(p, "\"name\""))){
        p = strchr(p, ':'); if(!p) break; p++;
        while(*p == ' ' || *p == '\"') p++;
        char name[256]; size_t i=0;
        while(*p && *p != '\"' && i < sizeof(name)-1) name[i++] = *p++;
        name[i] = '\0';
        if(i) printf("Cloud file: %s\n", name);
    }
}

static BOOL scan_onedrive(const char* token, Db* db){
    (void)db; // database integration TBD
    wchar_t headers[512];
    swprintf(headers, 512, L"Authorization: Bearer %S\r\n", token);
    char* resp = NULL;
    if(!http_request(L"graph.microsoft.com",
                     L"/v1.0/me/drive/root/children", L"GET",
                     headers, NULL, &resp)) return FALSE;
    process_json_listing(resp);
    free(resp);
    return TRUE;
}

static BOOL scan_google_drive(const char* token, Db* db){
    (void)db;
    wchar_t headers[512];
    swprintf(headers, 512, L"Authorization: Bearer %S\r\n", token);
    char* resp = NULL;
    if(!http_request(L"www.googleapis.com",
                     L"/drive/v3/files?fields=files(id,name,mimeType,modifiedTime,size)",
                     L"GET", headers, NULL, &resp)) return FALSE;
    process_json_listing(resp);
    free(resp);
    return TRUE;
}

static BOOL scan_pcloud(const char* token, Db* db){
    (void)db;
    wchar_t path[512];
    swprintf(path, 512, L"/listfolder?auth=%S&folderid=0", token);
    char* resp = NULL;
    if(!http_request(L"api.pcloud.com", path, L"GET", L"", NULL, &resp)) return FALSE;
    process_json_listing(resp);
    free(resp);
    return TRUE;
}

static BOOL scan_dropbox(const char* token, Db* db){
    (void)db;
    wchar_t headers[512];
    swprintf(headers, 512, L"Authorization: Bearer %S\r\nContent-Type: application/json\r\n", token);
    const char* body = "{\"path\":\"\",\"recursive\":false}";
    char* resp = NULL;
    if(!http_request(L"api.dropboxapi.com",
                     L"/2/files/list_folder", L"POST",
                     headers, body, &resp)) return FALSE;
    process_json_listing(resp);
    free(resp);
    return TRUE;
}

BOOL CloudScanner_Start(CloudProvider provider, const char* auth_token, Db* db){
    switch(provider){
    case CLOUD_ONEDRIVE:      return scan_onedrive(auth_token, db);
    case CLOUD_GOOGLE_DRIVE:  return scan_google_drive(auth_token, db);
    case CLOUD_PCLOUD:        return scan_pcloud(auth_token, db);
    case CLOUD_DROPBOX:       return scan_dropbox(auth_token, db);
    default: break;
    }
    return FALSE;
}
