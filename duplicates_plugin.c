#include "plugin.h"
#include "util.h"
#include <windows.h>
#include <stdint.h>
#include <stdio.h>

static PluginHost g_host;

static BOOL init(const PluginHost* host){
    if(!host) return FALSE;
    g_host = *host;
    return TRUE;
}

static void scan(void){
    // Placeholder duplicate scan implementation
}

static void shutdown(void){ }

static AnythingPlugin g_plugin = {
    ANYTHING_PLUGIN_API_VERSION,
    L"Duplicate Finder Plugin",
    init,
    scan,
    shutdown
};

__declspec(dllexport) AnythingPlugin* Anything_GetPlugin(void){ return &g_plugin; }
