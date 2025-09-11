#ifndef PLUGIN_H
#define PLUGIN_H

#include "anything.h"

#define ANYTHING_PLUGIN_API_VERSION 1

typedef struct PluginHost {
    MPMCQueue* queue;
    CancelToken* cancel_token;
} PluginHost;

typedef struct AnythingPlugin {
    uint32_t api_version;
    const wchar_t* name;
    BOOL (*init)(const PluginHost* host);
    void (*scan)(void);
    void (*shutdown)(void);
} AnythingPlugin;

typedef AnythingPlugin* (*Anything_GetPluginFn)(void);

void Plugin_LoadAll(const wchar_t* dir, PluginHost* host);
void Plugin_ScanAll(void);
void Plugin_UnloadAll(void);

#endif
