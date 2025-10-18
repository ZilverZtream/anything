#include "anything/plugin.h"
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
typedef HMODULE PluginModule;
#else
#include <dlfcn.h>
#include <dirent.h>
#include <string.h>
#include <wchar.h>
#include <limits.h>
#include <stdlib.h>
typedef void* PluginModule;
#endif

typedef struct {
    PluginModule module;
    AnythingPlugin* api;
} LoadedPlugin;

static LoadedPlugin g_plugins[16];
static size_t g_plugin_count = 0;
static PluginHost g_host;

void Plugin_LoadAll(const wchar_t* dir, PluginHost* host){
    g_host = *host;
#ifdef _WIN32
    wchar_t pattern[MAX_PATH];
    _snwprintf(pattern, MAX_PATH, L"%s\\*.dll", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if(h==INVALID_HANDLE_VALUE) return;
    do{
        wchar_t path[MAX_PATH];
        _snwprintf(path, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
        PluginModule mod = LoadLibraryW(path);
        if(!mod) continue;
        Anything_GetPluginFn getp = (Anything_GetPluginFn)GetProcAddress(mod, "Anything_GetPlugin");
        if(!getp){ FreeLibrary(mod); continue; }
        AnythingPlugin* api = getp();
        if(!api || api->api_version != ANYTHING_PLUGIN_API_VERSION){
            FreeLibrary(mod); continue;
        }
        if(api->init && !api->init(&g_host)){
            FreeLibrary(mod); continue;
        }
        g_plugins[g_plugin_count].module = mod;
        g_plugins[g_plugin_count].api = api;
        g_plugin_count++;
    }while(FindNextFileW(h, &fd) && g_plugin_count < 16);
    FindClose(h);
#else
    char dir_mb[PATH_MAX];
    wcstombs(dir_mb, dir, sizeof(dir_mb));
    DIR* d = opendir(dir_mb);
    if(!d) return;
    struct dirent* ent;
    while((ent = readdir(d)) && g_plugin_count < 16){
        const char* name = ent->d_name;
        size_t len = strlen(name);
        int is_shared = 0;
#ifdef __APPLE__
        if(len > 6 && strcmp(name + len - 6, ".dylib") == 0) is_shared = 1;
#else
        if(len > 3 && strcmp(name + len - 3, ".so") == 0) is_shared = 1;
#endif
        if(!is_shared) continue;
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dir_mb, name);
        PluginModule mod = dlopen(path, RTLD_NOW);
        if(!mod) continue;
        Anything_GetPluginFn getp = (Anything_GetPluginFn)dlsym(mod, "Anything_GetPlugin");
        if(!getp){ dlclose(mod); continue; }
        AnythingPlugin* api = getp();
        if(!api || api->api_version != ANYTHING_PLUGIN_API_VERSION){
            dlclose(mod); continue;
        }
        if(api->init && !api->init(&g_host)){
            dlclose(mod); continue;
        }
        g_plugins[g_plugin_count].module = mod;
        g_plugins[g_plugin_count].api = api;
        g_plugin_count++;
    }
    closedir(d);
#endif
}

void Plugin_ScanAll(void){
    for(size_t i=0;i<g_plugin_count;i++){
        if(g_plugins[i].api->scan)
            g_plugins[i].api->scan();
    }
}

void Plugin_UnloadAll(void){
    for(size_t i=0;i<g_plugin_count;i++){
        if(g_plugins[i].api->shutdown)
            g_plugins[i].api->shutdown();
#ifdef _WIN32
        FreeLibrary(g_plugins[i].module);
#else
        dlclose(g_plugins[i].module);
#endif
    }
    g_plugin_count = 0;
}

