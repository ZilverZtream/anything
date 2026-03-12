#include "core/pch.h"
#include <aclapi.h>

#ifdef ENTERPRISE

typedef struct EnterpriseSession {
    HANDLE token;
    char username[256];
} EnterpriseSession;

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

void* enterprise_ad_login(const char *user, const char *password){
    if(!user || !password) return NULL;
    if(user[0] == '\0') return NULL;

    HANDLE tok = NULL;
    if(!LogonUserA(user, NULL, password, LOGON32_LOGON_NETWORK,
                   LOGON32_PROVIDER_DEFAULT, &tok)){
        fprintf(stderr, "[enterprise] AD authentication failed for '%s' (err=%lu)\n",
                user, GetLastError());
        return NULL;
    }

    EnterpriseSession* session = (EnterpriseSession*)calloc(1, sizeof(EnterpriseSession));
    if(!session){
        CloseHandle(tok);
        return NULL;
    }
    session->token = tok;
    strncpy(session->username, user, sizeof(session->username) - 1);
    session->username[sizeof(session->username) - 1] = '\0';
    return session;
}

void enterprise_close_session(void* session_ptr){
    if(!session_ptr) return;
    EnterpriseSession* session = (EnterpriseSession*)session_ptr;
    if(session->token) CloseHandle(session->token);
    SecureZeroMemory(session, sizeof(EnterpriseSession));
    free(session);
}

const char* enterprise_session_user(void* session_ptr){
    if(!session_ptr) return "";
    return ((EnterpriseSession*)session_ptr)->username;
}

int enterprise_check_permission(void* session_ptr, const char *path){
    /* Fail-closed: no session or no path means no access. */
    if(!session_ptr || !path) return 0;

    EnterpriseSession* session = (EnterpriseSession*)session_ptr;
    if(!session->token) return 0;

    PSECURITY_DESCRIPTOR sd = NULL;
    DWORD err = GetNamedSecurityInfoA(path, SE_FILE_OBJECT,
                                      DACL_SECURITY_INFORMATION | OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION,
                                      NULL, NULL, NULL, NULL, &sd);
    if(err != ERROR_SUCCESS) return 0;

    DWORD desired = FILE_GENERIC_READ;
    GENERIC_MAPPING mapping = { FILE_GENERIC_READ, FILE_GENERIC_WRITE,
                                FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS };
    MapGenericMask(&desired, &mapping);
    PRIVILEGE_SET privs;
    DWORD privs_len = sizeof(privs);
    DWORD granted = 0;
    BOOL access = FALSE;
    if(!AccessCheck(sd, session->token, desired, &mapping,
                    &privs, &privs_len, &granted, &access)){
        access = FALSE;
    }

    LocalFree(sd);
    return access ? 1 : 0;
}

static void sanitize_log_field(const char *in, char *out, size_t outcch){
    size_t j = 0;
    for(size_t i = 0; in[i] && j < outcch - 1; ++i){
        unsigned char c = (unsigned char)in[i];
        if(c == '\t' || c == '\n' || c == '\r') out[j++] = ' ';
        else if(c >= 0x20) out[j++] = (char)c;
    }
    out[j] = '\0';
}

void enterprise_audit_log(const char *user, const char *query){
    if(!user || !query) return;
    char log_path[MAX_PATH];
    char* appdata = getenv("LOCALAPPDATA");
    if(appdata) snprintf(log_path, sizeof(log_path), "%s\\Anything\\audit.log", appdata);
    else strncpy(log_path, "audit.log", sizeof(log_path));
    FILE *f = fopen(log_path, "a");
    if(!f) return;
    char safe_user[256], safe_query[4096];
    sanitize_log_field(user, safe_user, sizeof(safe_user));
    sanitize_log_field(query, safe_query, sizeof(safe_query));
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d\t%s\t%s\n",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond,
            safe_user, safe_query);
    fclose(f);
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
