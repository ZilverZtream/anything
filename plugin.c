#include "plugin.h"
#include <stdio.h>

typedef struct {
    HMODULE module;
    AnythingPlugin* api;
} LoadedPlugin;

static LoadedPlugin g_plugins[16];
static size_t g_plugin_count = 0;
static PluginHost g_host;

void Plugin_LoadAll(const wchar_t* dir, PluginHost* host){
    g_host = *host;
    wchar_t pattern[MAX_PATH];
    _snwprintf(pattern, MAX_PATH, L"%s\\*.dll", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if(h==INVALID_HANDLE_VALUE) return;
    do{
        wchar_t path[MAX_PATH];
        _snwprintf(path, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
        HMODULE mod = LoadLibraryW(path);
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
        FreeLibrary(g_plugins[i].module);
    }
    g_plugin_count = 0;
}
