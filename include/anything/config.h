#pragma once

#ifndef CONFIG_H
#define CONFIG_H

#include "anything/anything.h"

typedef struct AppConfig {
    int default_index_threads;
    int max_index_threads;
    int default_batch;
    int default_search_workers;
    int max_search_workers;
} AppConfig;

extern AppConfig g_config;

void config_init_default(void);
BOOL config_load_file(const wchar_t* path);

#endif
