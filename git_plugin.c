// Git Repository Scanner Plugin
// Scans for .git directories and indexes commit messages and diffs.

#include "plugin.h"
#include "util.h"
#include <wchar.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#ifdef _WIN32
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#define _snwprintf swprintf
#define WAIT_OBJECT_0 0
static int WaitForSingleObject(HANDLE h, unsigned int ms){(void)h;(void)ms;return 1;}
#define Sleep(ms) usleep((ms)*1000)
static int wcscpy_s(wchar_t* dst, size_t dstcch, const wchar_t* src){
    if(!dst || !src || dstcch==0) return 1;
    wcsncpy(dst, src, dstcch);
    dst[dstcch-1] = 0;
    return 0;
}
#endif

static uint64_t to_filetime(time_t t){
    return ((uint64_t)t*10000000ULL)+116444736000000000ULL;
}

#ifdef _WIN32
#define POPEN  _popen
#define PCLOSE _pclose
#else
#define POPEN  popen
#define PCLOSE pclose
#endif

static PluginHost g_host;

static wchar_t* utf8_to_wchar(const char* s){
    if(!s) return NULL;
    size_t len = mbstowcs(NULL, s, 0);
    if(len==(size_t)-1) return NULL;
    wchar_t* w = (wchar_t*)malloc((len+1)*sizeof(wchar_t));
    if(!w) return NULL;
    mbstowcs(w, s, len+1);
    return w;
}

static void process_commit(const wchar_t* repo_path, const char* hash){
    char repo_mb[PATH_MAX];
    wcstombs(repo_mb, repo_path, sizeof(repo_mb));
    char cmd[PATH_MAX + 128];
    snprintf(cmd, sizeof(cmd), "git -C \"%s\" show --no-color --format=%%H%%n%%an%%n%%at%%n%%B --patch %s", repo_mb, hash);
    FILE* fp = POPEN(cmd, "r");
    if(!fp) return;
    char line[4096];
    if(!fgets(line, sizeof(line), fp)){ PCLOSE(fp); return; }
    char commit_hash[64];
    sscanf(line, "%63s", commit_hash);
    if(!fgets(line, sizeof(line), fp)){ PCLOSE(fp); return; }
    char author[256];
    line[strcspn(line, "\r\n")] = 0;
    strcpy(author, line);
    if(!fgets(line, sizeof(line), fp)){ PCLOSE(fp); return; }
    unsigned long long ts = strtoull(line, NULL, 10);
    char* content = NULL; size_t cap=0,len=0;
    while(fgets(line, sizeof(line), fp)){
        size_t l = strlen(line);
        if(len + l + 1 > cap){
            size_t new_cap = cap ? cap * 2 : 4096;
            while(new_cap < len + l + 1) new_cap *= 2;
            char* tmp = (char*)realloc(content, new_cap);
            if(!tmp){ free(content); content=NULL; break; }
            content = tmp; cap = new_cap;
        }
        memcpy(content + len, line, l);
        len += l;
    }
    if(content) content[len] = 0;
    PCLOSE(fp);

    size_t authlen = strlen(author);
    size_t contlen = content ? strlen(content) : 0;
    char* total = (char*)malloc(authlen + contlen + 10);
    if(total){
        int n = snprintf(total, authlen + contlen + 10, "Author: %s\n", author);
        if(content) memcpy(total + n, content, contlen + 1);
        else total[n] = 0;
    }
    free(content);
    wchar_t* wcontent = utf8_to_wchar(total);
    free(total);
    if(!wcontent) return;

    DbWorkItem* wi = (DbWorkItem*)aligned_malloc(sizeof(DbWorkItem), CACHE_LINE_SIZE);
    if(!wi){ free(wcontent); return; }
    wcscpy_s(wi->parent_path, MAX_LONG_PATH, repo_path);
    wchar_t whash[MAX_PATH];
    mbstowcs(whash, commit_hash, MAX_PATH);
    wcscpy_s(wi->name, MAX_PATH, whash);
    uint64_t ft = to_filetime((time_t)ts);
    wi->file_size = 0;
    wi->creation_time = ft;
    wi->modified_time = ft;
    wi->access_time = ft;
    wi->attributes = 0;
    wi->stage = INDEX_FULL_CONTENT;
    wi->op = WI_ADD;
    wi->content = wcontent;
    wi->preview = NULL;
    wi->clone_id = 0;
    while(!MPMC_Push(g_host.queue, wi)){
        if(WaitForSingleObject(g_host.cancel_event,0)==WAIT_OBJECT_0){
            aligned_free(wi); free(wcontent); return;
        }
        Sleep(0);
    }
}

#ifdef _WIN32
static void scan_dir(const wchar_t* dir){
    if(WaitForSingleObject(g_host.cancel_event,0)==WAIT_OBJECT_0) return;
    wchar_t pattern[MAX_LONG_PATH];
    _snwprintf(pattern, MAX_LONG_PATH, L"%s\\*.*", dir);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pattern, &fd);
    if(h==INVALID_HANDLE_VALUE) return;
    do{
        if(WaitForSingleObject(g_host.cancel_event,0)==WAIT_OBJECT_0) break;
        if(fd.cFileName[0]==L'.' && (fd.cFileName[1]==0 || (fd.cFileName[1]==L'.' && fd.cFileName[2]==0))) continue;
        wchar_t full[MAX_LONG_PATH];
        _snwprintf(full, MAX_LONG_PATH, L"%s\\%s", dir, fd.cFileName);
        if(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY){
            if(wcscmp(fd.cFileName, L".git")==0){
                char repo_mb[PATH_MAX];
                wcstombs(repo_mb, dir, sizeof(repo_mb));
                char cmd[PATH_MAX + 64];
                snprintf(cmd, sizeof(cmd), "git -C \"%s\" rev-list --all", repo_mb);
                FILE* fp = POPEN(cmd, "r");
                if(fp){
                    char hash[128];
                    while(fgets(hash,sizeof(hash),fp)){
                        if(WaitForSingleObject(g_host.cancel_event,0)==WAIT_OBJECT_0) break;
                        hash[strcspn(hash, "\r\n")] = 0;
                        if(hash[0]) process_commit(dir, hash);
                    }
                    PCLOSE(fp);
                }
            }else{
                scan_dir(full);
            }
        }
    }while(FindNextFileW(h,&fd));
    FindClose(h);
}
#else
static void scan_dir(const wchar_t* dir){
    if(WaitForSingleObject(g_host.cancel_event,0)==WAIT_OBJECT_0) return;
    char dir_mb[PATH_MAX];
    wcstombs(dir_mb, dir, sizeof(dir_mb));
    DIR* d = opendir(dir_mb);
    if(!d) return;
    struct dirent* ent;
    while((ent = readdir(d))){
        if(WaitForSingleObject(g_host.cancel_event,0)==WAIT_OBJECT_0) break;
        if(ent->d_name[0]=='.' && (ent->d_name[1]==0 || (ent->d_name[1]=='.' && ent->d_name[2]==0))) continue;
        char full_mb[PATH_MAX];
        snprintf(full_mb, sizeof(full_mb), "%s/%s", dir_mb, ent->d_name);
        struct stat st;
        if(stat(full_mb, &st)!=0) continue;
        wchar_t full[MAX_LONG_PATH];
        mbstowcs(full, full_mb, MAX_LONG_PATH);
        for(wchar_t* p=full; *p; ++p) if(*p==L'/') *p=L'\\';
        if(S_ISDIR(st.st_mode)){
            if(strcmp(ent->d_name, ".git")==0){
                // found git repository at parent path "dir"
                char repo_mb[PATH_MAX];
                wcstombs(repo_mb, dir, sizeof(repo_mb));
                char cmd[PATH_MAX + 64];
                snprintf(cmd, sizeof(cmd), "git -C \"%s\" rev-list --all", repo_mb);
                FILE* fp = POPEN(cmd, "r");
                if(fp){
                    char hash[128];
                    while(fgets(hash,sizeof(hash),fp)){
                        if(WaitForSingleObject(g_host.cancel_event,0)==WAIT_OBJECT_0) break;
                        hash[strcspn(hash, "\r\n")] = 0;
                        if(hash[0]) process_commit(dir, hash);
                    }
                    PCLOSE(fp);
                }
            }else{
                scan_dir(full);
            }
        }
    }
    closedir(d);
}
#endif

static BOOL init(const PluginHost* host){
    if(!host) return FALSE;
    g_host = *host;
    return TRUE;
}

static void scan(void){
    scan_dir(L".");
}

static void plugin_shutdown(void){
    // no state to free
}

static AnythingPlugin g_plugin = {
    ANYTHING_PLUGIN_API_VERSION,
    L"Git Repository Scanner",
    init,
    scan,
    plugin_shutdown
};

__declspec(dllexport) AnythingPlugin* Anything_GetPlugin(void){
    return &g_plugin;
}

