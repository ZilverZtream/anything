// System Registry Scanner Plugin
// Windows-only: indexes registry keys and string values for fast search.

#include "anything/plugin.h"
#include "anything/util.h"
#include <wchar.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>

static PluginHost g_host;

static BOOL init(const PluginHost* host){
    if(!host) return FALSE;
    g_host = *host;
    return TRUE;
}

static void push_item(const wchar_t* parent, const wchar_t* name, const wchar_t* content){
    DbWorkItem* wi = (DbWorkItem*)aligned_malloc(sizeof(DbWorkItem), CACHE_LINE_SIZE);
    if(!wi) return;
    wcscpy_s(wi->parent_path, MAX_LONG_PATH, parent ? parent : L"");
    wcscpy_s(wi->name, MAX_PATH, name ? name : L"");
    wi->file_size = 0;
    wi->creation_time = 0;
    wi->modified_time = 0;
    wi->access_time = 0;
    wi->attributes = 0;
    wi->stage = content ? INDEX_FULL_CONTENT : INDEX_NAMES_ONLY;
    wi->op = WI_ADD;
    if(content){
        size_t len = wcslen(content) + 1;
        wi->content = (wchar_t*)malloc(sizeof(wchar_t) * len);
        if(wi->content) wcscpy_s(wi->content, len, content);
    }else{
        wi->content = NULL;
    }
    wi->preview = NULL;
    wi->clone_id = 0;

    int tries = 0;
    while(!MPMC_Push(g_host.queue, wi)){
        if(is_cancelled(g_host.cancel_token) || tries++>1000){
            if(wi->content) free(wi->content);
            aligned_free(wi);
            return;
        }
        Sleep(0);
    }
}

static void scan_key(HKEY key, const wchar_t* path){
    if(is_cancelled(g_host.cancel_token)) return;

    DWORD val_count=0, max_val_name=0, max_val_len=0;
    if(RegQueryInfoKeyW(key,NULL,NULL,NULL,NULL,NULL,NULL,&val_count,&max_val_name,&max_val_len,NULL,NULL)!=ERROR_SUCCESS){
        fwprintf(stderr,L"[registry] RegQueryInfoKeyW failed for %ls: %lu\n", path, GetLastError());
        return;
    }
    wchar_t* val_name = (wchar_t*)malloc(sizeof(wchar_t)*(max_val_name+2));
    BYTE*   val_data = (BYTE*)malloc(max_val_len+2*sizeof(wchar_t));
    if(!val_name || !val_data){
        fwprintf(stderr,L"[registry] allocation failed for %ls\n", path);
    }
    if(val_name && val_data){
        for(DWORD i=0;i<val_count;i++){
            DWORD name_len = max_val_name+1;
            DWORD data_len = max_val_len;
            DWORD type = 0;
            if(RegEnumValueW(key,i,val_name,&name_len,NULL,&type,val_data,&data_len)!=ERROR_SUCCESS){
                fwprintf(stderr,L"[registry] RegEnumValueW failed in %ls: %lu\n", path, GetLastError());
                continue;
            }
            const wchar_t* vname = name_len ? val_name : L"(Default)";
            wchar_t* content = NULL;
            if(type==REG_SZ || type==REG_EXPAND_SZ){
                content = (wchar_t*)val_data;
            }else if(type==REG_MULTI_SZ){
                size_t wc = data_len/sizeof(wchar_t);
                for(size_t j=0;j<wc-1;j++) if(((wchar_t*)val_data)[j]==0) ((wchar_t*)val_data)[j]=L' ';
                content = (wchar_t*)val_data;
            }
            push_item(path, vname, content);
        }
    }
    free(val_name); free(val_data);

    DWORD sub_count=0, max_sub_name=0;
    if(RegQueryInfoKeyW(key,NULL,NULL,NULL,&sub_count,&max_sub_name,NULL,NULL,NULL,NULL,NULL,NULL)!=ERROR_SUCCESS){
        fwprintf(stderr,L"[registry] RegQueryInfoKeyW failed for %ls: %lu\n", path, GetLastError());
        return;
    }
    wchar_t* sub_name = (wchar_t*)malloc(sizeof(wchar_t)*(max_sub_name+2));
    if(sub_name){
        for(DWORD i=0;i<sub_count;i++){
            DWORD namelen = max_sub_name+1;
            if(RegEnumKeyExW(key,i,sub_name,&namelen,NULL,NULL,NULL,NULL)!=ERROR_SUCCESS){
                fwprintf(stderr,L"[registry] RegEnumKeyExW failed in %ls: %lu\n", path, GetLastError());
                continue;
            }
            wchar_t sub_path[MAX_LONG_PATH];
            _snwprintf(sub_path, MAX_LONG_PATH, L"%s\\%s", path, sub_name);
            push_item(path, sub_name, NULL);
            HKEY child;
            if(RegOpenKeyExW(key, sub_name,0,KEY_READ,&child)==ERROR_SUCCESS){
                scan_key(child, sub_path);
                RegCloseKey(child);
            }
        }
    }
    free(sub_name);
}

static void scan(void){
    struct { HKEY root; const wchar_t* name; } roots[] = {
        {HKEY_LOCAL_MACHINE, L"HKLM"},
        {HKEY_CURRENT_USER,  L"HKCU"},
        {HKEY_CLASSES_ROOT,  L"HKCR"},
        {HKEY_USERS,         L"HKU"},
        {HKEY_CURRENT_CONFIG,L"HKCC"},
    };
    for(size_t i=0;i<sizeof(roots)/sizeof(roots[0]);i++){
        push_item(L"", roots[i].name, NULL);
        HKEY h;
        if(RegOpenKeyExW(roots[i].root, NULL, 0, KEY_READ, &h)==ERROR_SUCCESS){
            scan_key(h, roots[i].name);
            RegCloseKey(h);
        }else{
            fwprintf(stderr,L"[registry] RegOpenKeyExW failed for %ls: %lu\n", roots[i].name, GetLastError());
        }
    }
}

static void plugin_shutdown(void){ }

static AnythingPlugin g_plugin = {
    ANYTHING_PLUGIN_API_VERSION,
    L"System Registry Scanner",
    init,
    scan,
    plugin_shutdown
};

__declspec(dllexport) AnythingPlugin* Anything_GetPlugin(void){
    return &g_plugin;
}

#endif // _WIN32
