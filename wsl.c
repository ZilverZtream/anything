// wsl.c - simple WSL filesystem scanner implementation
#include "wsl.h"
#include <stdio.h>
#include <stdlib.h>

// Simple helper to append a child path to a root
static void join_path(wchar_t* dst, size_t dst_sz, const wchar_t* root, const wchar_t* rel){
    wcscpy_s(dst, dst_sz, root);
    size_t n = wcslen(dst);
    if(n && dst[n-1] != L'\\') wcscat_s(dst, dst_sz, L"\\");
    // skip leading '/'
    if(rel[0] == L'/') rel++;
    while(*rel){
        wchar_t c = *rel++;
        dst[n++] = (c == L'/') ? L'\\' : c;
        dst[n] = 0;
        if(n+1 >= dst_sz) break;
    }
}

BOOL WSLScanner_Start(const wchar_t* root_path, Db* db){
    if(!root_path || !db) return FALSE;

    // Derive distro name from \\wsl$\Distro format
    const wchar_t* distro = root_path;
    const wchar_t* slash = wcsrchr(root_path, L'\\');
    if(slash) distro = slash + 1;

    wchar_t cmd[512];
    _snwprintf(cmd, 512, L"wsl.exe -d \"%s\" -- find / -print0", distro);

    FILE* pipe = _wpopen(cmd, L"rb");
    if(!pipe) return FALSE;

    if(!db_begin_write(db)) { _pclose(pipe); return FALSE; }

    char buf[4096];
    char path[65536];
    size_t path_len = 0;
    BOOL ok = TRUE;

    while(!feof(pipe)){
        size_t got = fread(buf, 1, sizeof(buf), pipe);
        for(size_t i=0;i<got;i++){
            if(buf[i] == '\0'){
                if(path_len){
                    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, (int)path_len, NULL, 0);
                    if(wlen > 0 && wlen < MAX_LONG_PATH){
                        wchar_t wpath[MAX_LONG_PATH];
                        MultiByteToWideChar(CP_UTF8, 0, path, (int)path_len, wpath, wlen);
                        wpath[wlen] = 0;

                        wchar_t full[MAX_LONG_PATH];
                        join_path(full, MAX_LONG_PATH, root_path, wpath);

                        wchar_t* name = wcsrchr(full, L'\\');
                        if(name){
                            *name = 0;
                            name++;
                            DbRecord rec = {0};
                            rec.type = DB_REC_FILE;
                            rec.parent_str_id = db_intern_wstring(db, full);
                            rec.name_str_id   = db_intern_wstring(db, name);
                            if(!db_put_records(db, &rec, 1)){
                                ok = FALSE;
                                break;
                            }
                        }
                    }
                }
                path_len = 0;
                if(!ok) break;
            } else if(path_len < sizeof(path)-1){
                path[path_len++] = buf[i];
            }
        }
        if(!ok) break;
    }

    if(ok) db_commit_write(db); else db_abort_write(db);
    _pclose(pipe);
    return ok;
}
