#include "enterprise.h"
#include "util.h"
#include <stdio.h>
#include <windows.h>

#ifdef ENTERPRISE

static void index_share_recursive(const wchar_t* path){
    wchar_t pattern[MAX_LONG_PATH];
    _snwprintf(pattern, MAX_LONG_PATH, L"%s\\*.*", path);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pattern, &fd);
    if(h==INVALID_HANDLE_VALUE) return;
    do{
        if(fd.cFileName[0]==L'.' && (fd.cFileName[1]==0 || (fd.cFileName[1]==L'.' && fd.cFileName[2]==0))) continue;
        wchar_t full[MAX_LONG_PATH];
        _snwprintf(full, MAX_LONG_PATH, L"%s\\%s", path, fd.cFileName);
        if(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY){
            index_share_recursive(full);
        } else {
            wprintf(L"%s\n", full);
        }
    }while(FindNextFileW(h,&fd));
    FindClose(h);
}

void enterprise_index_network(const char *share){
    if(!share) return;
    wchar_t wshare[MAX_LONG_PATH];
    to_wide(share, wshare, MAX_LONG_PATH);
    index_share_recursive(wshare);
}

int enterprise_check_permission(const char *user, const char *path){
    (void)user;
    if(!path) return 0;
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if(h==INVALID_HANDLE_VALUE){
        return 0;
    }
    CloseHandle(h);
    return 1;
}

void enterprise_audit_log(const char *user, const char *query){
    FILE *f = fopen("audit.log", "a");
    if(!f) return;
    fprintf(f, "%s\t%s\n", user, query);
    fclose(f);
}

int enterprise_ad_authenticate(const char *user, const char *password){
    HANDLE tok=NULL;
    if(LogonUserA(user, NULL, password, LOGON32_LOGON_NETWORK, LOGON32_PROVIDER_DEFAULT, &tok)){
        CloseHandle(tok);
        return 1;
    }
    return 0;
}

void enterprise_deploy_msi(void){
    char cmd[MAX_PATH*2];
    snprintf(cmd, sizeof(cmd), "msiexec /i \"%s\" /qn", "anything_enterprise.msi");
    STARTUPINFOA si; PROCESS_INFORMATION pi;
    ZeroMemory(&si,sizeof(si)); ZeroMemory(&pi,sizeof(pi)); si.cb = sizeof(si);
    if(CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)){
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        fprintf(stderr, "[enterprise] MSI deployment failed (%lu)\n", GetLastError());
    }
}

#endif /* ENTERPRISE */
