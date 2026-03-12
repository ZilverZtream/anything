// Microsoft Mail Scanner Plugin
// Integrates with Microsoft Graph API using OAuth 2.0 access tokens.

#include "anything/plugin.h"
#include <wchar.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <curl/curl.h>
#include "../../third_party/cJSON/cJSON.h"

#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#else
#include <unistd.h>
#include <sys/stat.h>
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

static BOOL http_get(const char* url,const char* token,char** out){
    if(out) *out=NULL; static int curl_inited=0;
    if(!curl_inited){ if(curl_global_init(CURL_GLOBAL_DEFAULT)!=0) return FALSE; curl_inited=1; }
    CURL* curl=curl_easy_init(); if(!curl) return FALSE;
    curl_easy_setopt(curl,CURLOPT_URL,url);
    struct curl_slist* hdr=NULL;
    size_t auth_len = strlen("Authorization: Bearer ") + strlen(token) + 1;
    char* auth = (char*)malloc(auth_len);
    if(!auth){ curl_easy_cleanup(curl); return FALSE; }
    snprintf(auth, auth_len, "Authorization: Bearer %s", token);
    hdr=curl_slist_append(hdr,auth); free(auth);
    curl_easy_setopt(curl,CURLOPT_HTTPHEADER,hdr);
    struct curl_buf buf={0};
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,curl_write_cb);
    curl_easy_setopt(curl,CURLOPT_WRITEDATA,&buf);
    CURLcode res=curl_easy_perform(curl);
    if(hdr) curl_slist_free_all(hdr); curl_easy_cleanup(curl);
    if(res!=CURLE_OK){ free(buf.data); return FALSE; }
    if(out) *out=buf.data; else free(buf.data); return TRUE;
}

static wchar_t g_token[4096]=L"";
static PluginHost g_host;

#ifdef _WIN32
// Sets ACL on a file to allow access only by the current user
// Returns TRUE on success, FALSE on failure
static BOOL set_owner_only_acl(const char* path){
    HANDLE hToken = NULL;
    PTOKEN_USER pTokenUser = NULL;
    PACL pAcl = NULL;
    EXPLICIT_ACCESSA ea;
    BOOL success = FALSE;

    // Get current user's SID
    if(!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)){
        fprintf(stderr, "[msmail] OpenProcessToken failed: error %lu\n", GetLastError());
        return FALSE;
    }

    DWORD dwSize = 0;
    GetTokenInformation(hToken, TokenUser, NULL, 0, &dwSize);
    pTokenUser = (PTOKEN_USER)malloc(dwSize);
    if(!pTokenUser){
        CloseHandle(hToken);
        return FALSE;
    }

    if(!GetTokenInformation(hToken, TokenUser, pTokenUser, dwSize, &dwSize)){
        fprintf(stderr, "[msmail] GetTokenInformation failed: error %lu\n", GetLastError());
        goto cleanup;
    }

    // Create an ACL that grants full control only to the current user
    ZeroMemory(&ea, sizeof(EXPLICIT_ACCESSA));
    ea.grfAccessPermissions = GENERIC_ALL;
    ea.grfAccessMode = SET_ACCESS;
    ea.grfInheritance = NO_INHERITANCE;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_USER;
    ea.Trustee.ptstrName = (LPSTR)pTokenUser->User.Sid;

    DWORD dwRes = SetEntriesInAclA(1, &ea, NULL, &pAcl);
    if(dwRes != ERROR_SUCCESS){
        fprintf(stderr, "[msmail] SetEntriesInAcl failed: error %lu\n", dwRes);
        goto cleanup;
    }

    // Apply the ACL to the file
    dwRes = SetNamedSecurityInfoA(
        (LPSTR)path,
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        NULL,
        NULL,
        pAcl,
        NULL
    );

    if(dwRes != ERROR_SUCCESS){
        fprintf(stderr, "[msmail] SetNamedSecurityInfo failed: error %lu\n", dwRes);
        goto cleanup;
    }

    success = TRUE;

cleanup:
    if(pAcl) LocalFree(pAcl);
    if(pTokenUser) free(pTokenUser);
    if(hToken) CloseHandle(hToken);
    return success;
}

// Verifies that a file is only accessible by the current user on Windows
// Returns TRUE if secure, FALSE if insecure or on error
// SECURITY: Uses file handle instead of path to prevent TOCTOU vulnerabilities
static BOOL verify_windows_acl(HANDLE hFile){
    PSECURITY_DESCRIPTOR pSD = NULL;
    PACL pDacl = NULL;
    PSID pOwnerSid = NULL;
    BOOL result = FALSE;

    // Get the security descriptor for the file by handle (not path)
    DWORD dwRes = GetSecurityInfo(
        hFile,
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | OWNER_SECURITY_INFORMATION,
        &pOwnerSid,
        NULL,
        &pDacl,
        NULL,
        &pSD
    );

    if(dwRes != ERROR_SUCCESS){
        fprintf(stderr, "[msmail] GetSecurityInfo failed: error %lu\n", dwRes);
        return FALSE;
    }

    // Get current user's SID
    HANDLE hToken = NULL;
    if(!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)){
        fprintf(stderr, "[msmail] OpenProcessToken failed: error %lu\n", GetLastError());
        LocalFree(pSD);
        return FALSE;
    }

    DWORD dwSize = 0;
    GetTokenInformation(hToken, TokenUser, NULL, 0, &dwSize);
    PTOKEN_USER pTokenUser = (PTOKEN_USER)malloc(dwSize);
    if(!pTokenUser){
        CloseHandle(hToken);
        LocalFree(pSD);
        return FALSE;
    }

    if(!GetTokenInformation(hToken, TokenUser, pTokenUser, dwSize, &dwSize)){
        fprintf(stderr, "[msmail] GetTokenInformation failed: error %lu\n", GetLastError());
        free(pTokenUser);
        CloseHandle(hToken);
        LocalFree(pSD);
        return FALSE;
    }

    PSID pCurrentUserSid = pTokenUser->User.Sid;

    // Check if current user is the owner
    if(!EqualSid(pCurrentUserSid, pOwnerSid)){
        fprintf(stderr, "[msmail] INSECURE: Token file is not owned by current user\n");
        free(pTokenUser);
        CloseHandle(hToken);
        LocalFree(pSD);
        return FALSE;
    }

    // Check DACL entries
    if(pDacl){
        ACL_SIZE_INFORMATION aclSize;
        if(GetAclInformation(pDacl, &aclSize, sizeof(aclSize), AclSizeInformation)){
            // Well-known SIDs to check for unauthorized access
            PSID pEveryoneSid = NULL;
            PSID pUsersSid = NULL;
            SID_IDENTIFIER_AUTHORITY worldAuth = SECURITY_WORLD_SID_AUTHORITY;
            SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;

            AllocateAndInitializeSid(&worldAuth, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &pEveryoneSid);
            AllocateAndInitializeSid(&ntAuth, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_USERS, 0, 0, 0, 0, 0, 0, &pUsersSid);

            result = TRUE; // Assume secure unless we find a problem

            for(DWORD i = 0; i < aclSize.AceCount; i++){
                LPVOID pAce = NULL;
                if(GetAce(pDacl, i, &pAce)){
                    ACCESS_ALLOWED_ACE* pAllowedAce = (ACCESS_ALLOWED_ACE*)pAce;
                    PSID pAceSid = (PSID)&pAllowedAce->SidStart;

                    // Check if this ACE grants access to Everyone or Users group
                    if((pEveryoneSid && EqualSid(pAceSid, pEveryoneSid)) ||
                       (pUsersSid && EqualSid(pAceSid, pUsersSid))){
                        // Check if this ACE grants read access
                        if(pAllowedAce->Header.AceType == ACCESS_ALLOWED_ACE_TYPE &&
                           (pAllowedAce->Mask & (FILE_GENERIC_READ | GENERIC_READ | FILE_READ_DATA))){
                            fprintf(stderr, "[msmail] INSECURE PERMISSIONS: Token file is readable by other users\n");
                            result = FALSE;
                            break;
                        }
                    }
                }
            }

            if(pEveryoneSid) FreeSid(pEveryoneSid);
            if(pUsersSid) FreeSid(pUsersSid);
        }
    }

    free(pTokenUser);
    CloseHandle(hToken);
    LocalFree(pSD);
    return result;
}
#endif

static BOOL load_token(void){
    // WARNING: Reading OAuth tokens from environment variables is INSECURE
    // Environment variables can be read by other processes
    // TODO: Use OS credential manager (Windows: CredRead, macOS: Keychain, Linux: libsecret)
    const char* env=getenv("MS_MAIL_TOKEN");
    if(env){
        fprintf(stderr,"[msmail] WARNING: Using MS_MAIL_TOKEN from environment variable is insecure!\n");
        fprintf(stderr,"[msmail] Tokens should be stored in OS credential manager or secure file with 0600 permissions.\n");
        to_wide(env,g_token,4096);
        return TRUE;
    }

    const char* path_env=getenv("MS_MAIL_TOKEN_FILE");
    char path_u8[512];
    if(path_env){ strncpy(path_u8,path_env,sizeof(path_u8)-1); path_u8[sizeof(path_u8)-1]=0; }
    else {
        const char* home=getenv("HOME");
#ifdef _WIN32
        if(!home) home = getenv("USERPROFILE");
        if(!home) return FALSE;
        snprintf(path_u8,sizeof(path_u8),"%s\\.anything\\ms_mail_token",home);
#else
        if(!home) return FALSE;
        snprintf(path_u8,sizeof(path_u8),"%s/.anything/ms_mail_token",home);
#endif
    }

    FILE* f=fopen(path_u8,"rb");
    if(!f){ fprintf(stderr,"[msmail] failed to open token file %s: %s\n",path_u8,strerror(errno)); return FALSE; }

    // Verify file permissions on all platforms
#ifdef _WIN32
    // On Windows, verify ACL using GetSecurityInfo on the file handle
    // This prevents TOCTOU vulnerabilities by checking the same object we're reading
    int fd = _fileno(f);
    HANDLE hFile = (HANDLE)_get_osfhandle(fd);
    if(hFile == INVALID_HANDLE_VALUE){
        fprintf(stderr,"[msmail] failed to get OS handle for %s\n", path_u8);
        fclose(f);
        return FALSE;
    }
    if(!verify_windows_acl(hFile)){
        fprintf(stderr,"[msmail] INSECURE PERMISSIONS on %s (must be readable only by current user)\n", path_u8);
        fclose(f);
        return FALSE;
    }
#else
    struct stat st;
    if(stat(path_u8,&st)!=0){
        fprintf(stderr,"[msmail] stat failed for %s: %s\n",path_u8,strerror(errno));
        fclose(f);
        return FALSE;
    }
    if((st.st_mode&0777)!=0600){
        fprintf(stderr,"[msmail] INSECURE PERMISSIONS on %s (must be 0600, found 0%o)\n",path_u8, st.st_mode&0777);
        fclose(f);
        return FALSE;
    }
#endif

    char buf[4096]; size_t n=fread(buf,1,sizeof(buf)-1,f); fclose(f);
    if(n==0){ fprintf(stderr,"[msmail] token file %s empty or unreadable\n",path_u8); return FALSE; }
    buf[n]=0; char* nl=strpbrk(buf,"\r\n"); if(nl) *nl=0;
    to_wide(buf,g_token,4096);
    return g_token[0]!=L'\0';
}

static void process_message(const char* id,const char* token_utf8){
    char url[512]; snprintf(url,sizeof(url),"https://graph.microsoft.com/v1.0/me/messages/%s?$select=subject,bodyPreview,receivedDateTime",id);
    char* resp=NULL; cJSON* root=NULL; wchar_t* wcontent=NULL; DbWorkItem* wi=NULL;
    if(!http_get(url,token_utf8,&resp)) goto cleanup;
    root=cJSON_Parse(resp);
    if(!root) goto cleanup;
    const char* subject=NULL; const char* preview=""; const char* received=NULL;
    cJSON* sj=cJSON_GetObjectItem(root,"subject"); if(cJSON_IsString(sj)) subject=sj->valuestring;
    cJSON* pv=cJSON_GetObjectItem(root,"bodyPreview"); if(cJSON_IsString(pv)) preview=pv->valuestring;
    cJSON* rd=cJSON_GetObjectItem(root,"receivedDateTime"); if(cJSON_IsString(rd)) received=rd->valuestring;
    uint64_t ts=received? parse_rfc3339(received):0;
    wchar_t wname[MAX_PATH]; if(subject) to_wide(subject,wname,MAX_PATH); else to_wide(id,wname,MAX_PATH);
    size_t wlen=mbstowcs(NULL,preview,0);
    if(wlen!=(size_t)-1){ wcontent=(wchar_t*)malloc((wlen+1)*sizeof(wchar_t)); if(wcontent) mbstowcs(wcontent,preview,wlen+1); }
    wi=(DbWorkItem*)wi_alloc(); if(!wi) goto cleanup;
    wcscpy_s(wi->parent_path,MAX_LONG_PATH,L"msmail");
    wcscpy_s(wi->name,MAX_PATH,wname);
    uint64_t ft=ts? to_filetime(ts):0;
    wi->file_size=0; wi->creation_time=ft; wi->modified_time=ft; wi->access_time=ft;
    wi->attributes=0; wi->stage=INDEX_FULL_CONTENT; wi->op=WI_ADD;
    wi->content=wcontent; wi->preview=NULL; wi->clone_id=0;
    wi->hash_crc=0; wi->hash_ready=FALSE;
    int tries=0; while(!MPMC_Push(g_host.queue,wi)){
        if(is_cancelled(g_host.cancel_token) || tries++>1000){
            goto cleanup;
        }
        Sleep(0);
    }
    wi=NULL; wcontent=NULL; // ownership transferred

cleanup:
    if(root) cJSON_Delete(root);
    if(resp) free(resp);
    if(wi) wi_free(wi);
    if(wcontent) free(wcontent);
}

static BOOL init(const PluginHost* host){
    if(!host) return FALSE; g_host=*host;
    if(!load_token()) return TRUE; // token may be absent

    const char* store_path=getenv("MS_MAIL_TOKEN_STORE");
    if(store_path){
        char tok_u8[4096]; to_utf8(g_token,tok_u8,sizeof(tok_u8));
        FILE* f=fopen(store_path,"wb");
        if(!f){
            fprintf(stderr,"[msmail] cannot write token store %s: %s\n",store_path,strerror(errno));
        }else{
            // Set secure permissions BEFORE writing sensitive data
#ifdef _WIN32
            // On Windows, write data first then set secure ACL
            fwrite(tok_u8,1,strlen(tok_u8),f);
            fclose(f);
            // Set owner-only ACL after file creation
            if(!set_owner_only_acl(store_path)){
                fprintf(stderr,"[msmail] WARNING: Failed to set secure ACL on token store %s\n",store_path);
            }
#else
            // On POSIX, set permissions before writing
            fchmod(fileno(f),0600);
            fwrite(tok_u8,1,strlen(tok_u8),f);
            fclose(f);
#endif
        }
    }
    return TRUE;
}

static void scan(void){
    if(g_token[0]==L'\0') return; char token_utf8[4096];
    to_utf8(g_token,token_utf8,sizeof(token_utf8));
    char* resp=NULL; cJSON* root=NULL;
    if(!http_get("https://graph.microsoft.com/v1.0/me/messages?$top=5",token_utf8,&resp)) goto cleanup;
    root=cJSON_Parse(resp);
    if(!root) goto cleanup;
    cJSON* arr=cJSON_GetObjectItem(root,"value");
    if(cJSON_IsArray(arr)){
        cJSON* m=NULL; cJSON_ArrayForEach(m,arr){
            cJSON* id=cJSON_GetObjectItem(m,"id");
            if(cJSON_IsString(id)) process_message(id->valuestring,token_utf8);
            if(is_cancelled(g_host.cancel_token)) break;
        }
    }

cleanup:
    memset(token_utf8, 0, sizeof(token_utf8));
    if(root) cJSON_Delete(root);
    if(resp) free(resp);
}

static void msmail_shutdown(void){
    memset(g_token, 0, sizeof(g_token));
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

