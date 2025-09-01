// Git Repository Scanner Plugin
// Scans for .git directories and indexes commit messages and diffs using libgit2.

#include "plugin.h"
#include "util.h"
#include <wchar.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <git2.h>

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

static void process_commit(const wchar_t* repo_path, git_repository* repo, const git_oid* oid){
    git_commit* commit = NULL;
    if(git_commit_lookup(&commit, repo, oid)!=0) return;

    const git_signature* sig = git_commit_author(commit);
    const char* author = sig && sig->name ? sig->name : "";
    git_time_t ts = git_commit_time(commit);
    const char* message = git_commit_message(commit);

    git_tree* commit_tree = NULL;
    git_tree* parent_tree = NULL;
    git_diff* diff = NULL;
    git_commit_tree(&commit_tree, commit);
    if(git_commit_parentcount(commit) > 0){
        git_commit* parent = NULL;
        git_commit_parent(&parent, commit, 0);
        git_commit_tree(&parent_tree, parent);
        git_commit_free(parent);
    }
    git_diff_tree_to_tree(&diff, repo, parent_tree, commit_tree, NULL);
    git_buf buf = GIT_BUF_INIT;
    git_diff_to_buf(&buf, diff, GIT_DIFF_FORMAT_PATCH);

    size_t authlen = strlen(author);
    size_t msglen = message ? strlen(message) : 0;
    size_t difflen = buf.size;
    char* total = (char*)malloc(authlen + msglen + difflen + 20);
    if(total){
        int n = snprintf(total, authlen + msglen + difflen + 20, "Author: %s\n", author);
        if(message) memcpy(total + n, message, msglen);
        n += (int)msglen;
        if(difflen){
            memcpy(total + n, buf.ptr, difflen);
            total[n + difflen] = 0;
        }else{
            total[n] = 0;
        }
    }
    git_buf_dispose(&buf);
    git_diff_free(diff);
    git_tree_free(commit_tree);
    git_tree_free(parent_tree);

    wchar_t* wcontent = utf8_to_wchar(total);
    free(total);
    if(!wcontent){ git_commit_free(commit); return; }

    char oid_str[GIT_OID_HEXSZ+1];
    git_oid_tostr(oid_str, sizeof(oid_str), oid);

    DbWorkItem* wi = (DbWorkItem*)aligned_malloc(sizeof(DbWorkItem), CACHE_LINE_SIZE);
    if(!wi){ git_commit_free(commit); free(wcontent); return; }
    wcscpy_s(wi->parent_path, MAX_LONG_PATH, repo_path);
    wchar_t whash[MAX_PATH];
    mbstowcs(whash, oid_str, MAX_PATH);
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
            aligned_free(wi); free(wcontent); git_commit_free(commit); return;
        }
        Sleep(0);
    }
    git_commit_free(commit);
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
                git_repository* repo = NULL;
                if(git_repository_open(&repo, repo_mb)==0){
                    git_revwalk* walk = NULL;
                    if(git_revwalk_new(&walk, repo)==0){
                        git_revwalk_sorting(walk, GIT_SORT_TIME);
                        git_reference_iterator* iter = NULL;
                        git_reference* ref = NULL;
                        if(git_reference_iterator_new(&iter, repo)==0){
                            while(git_reference_next(&ref, iter)==0){
                                const git_oid* oid = git_reference_target(ref);
                                if(oid) git_revwalk_push(walk, oid);
                                git_reference_free(ref);
                            }
                            git_reference_iterator_free(iter);
                        }
                        git_oid oid;
                        while(git_revwalk_next(&oid, walk)==0){
                            if(WaitForSingleObject(g_host.cancel_event,0)==WAIT_OBJECT_0) break;
                            process_commit(dir, repo, &oid);
                        }
                        git_revwalk_free(walk);
                    }
                    git_repository_free(repo);
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
                char repo_mb[PATH_MAX];
                wcstombs(repo_mb, dir, sizeof(repo_mb));
                git_repository* repo = NULL;
                if(git_repository_open(&repo, repo_mb)==0){
                    git_revwalk* walk = NULL;
                    if(git_revwalk_new(&walk, repo)==0){
                        git_revwalk_sorting(walk, GIT_SORT_TIME);
                        git_reference_iterator* iter = NULL;
                        git_reference* ref = NULL;
                        if(git_reference_iterator_new(&iter, repo)==0){
                            while(git_reference_next(&ref, iter)==0){
                                const git_oid* oid = git_reference_target(ref);
                                if(oid) git_revwalk_push(walk, oid);
                                git_reference_free(ref);
                            }
                            git_reference_iterator_free(iter);
                        }
                        git_oid oid;
                        while(git_revwalk_next(&oid, walk)==0){
                            if(WaitForSingleObject(g_host.cancel_event,0)==WAIT_OBJECT_0) break;
                            process_commit(dir, repo, &oid);
                        }
                        git_revwalk_free(walk);
                    }
                    git_repository_free(repo);
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
    git_libgit2_init();
    return TRUE;
}

static void scan(void){
    scan_dir(L".");
}

static void plugin_shutdown(void){
    git_libgit2_shutdown();
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

