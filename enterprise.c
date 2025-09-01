#include "enterprise.h"
#include "util.h"
#include "anything.h"
#include <stdio.h>
#include <windows.h>
#include <aclapi.h>

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
    (void)user; // In a full implementation we would evaluate this specific user's token
    if(!path) return 0;

    PSECURITY_DESCRIPTOR sd = NULL;
    DWORD err = GetNamedSecurityInfoA(path, SE_FILE_OBJECT,
                                      DACL_SECURITY_INFORMATION,
                                      NULL, NULL, NULL, NULL, &sd);
    if(err != ERROR_SUCCESS) return 0;

    HANDLE token = NULL;
    if(!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)){
        LocalFree(sd);
        return 0;
    }

    DWORD desired = FILE_GENERIC_READ;
    GENERIC_MAPPING mapping = { FILE_GENERIC_READ, FILE_GENERIC_WRITE,
                                FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS };
    MapGenericMask(&desired, &mapping);
    PRIVILEGE_SET privs; DWORD privs_len = sizeof(privs); DWORD granted = 0; BOOL access = FALSE;
    if(!AccessCheck(sd, token, desired, &mapping, &privs, &privs_len, &granted, &access)){
        access = FALSE;
    }

    CloseHandle(token);
    LocalFree(sd);
    return access ? 1 : 0;
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
