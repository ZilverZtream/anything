#include "config.h"
#include <wchar.h>
#include <stdio.h>
#include <stdlib.h>

AppConfig g_config;

static FILE* cfg_fopen(const wchar_t* path, const wchar_t* mode){
#ifdef _WIN32
    return _wfopen(path, mode);
#else
    char path_mb[1024];
    char mode_mb[16];
    wcstombs(path_mb, path, sizeof(path_mb));
    wcstombs(mode_mb, mode, sizeof(mode_mb));
    return fopen(path_mb, mode_mb);
#endif
}

static void trim_newline(wchar_t* s){
    for(wchar_t* p=s; *p; ++p){
        if(*p=='\r' || *p=='\n'){ *p=0; break; }
    }
}

void config_init_default(void){
    g_config.default_index_threads = 8;
    g_config.max_index_threads = MAX_THREADS;
    g_config.default_batch = 50000;
    g_config.default_search_workers = 1;
    g_config.max_search_workers = 4;
}

BOOL config_load_file(const wchar_t* path){
    FILE* f = cfg_fopen(path, L"rt");
    if(!f) return FALSE;
    wchar_t line[256];
    while(fgetws(line, sizeof(line)/sizeof(line[0]), f)){
        wchar_t* eq = wcschr(line, L'=');
        if(!eq) continue;
        *eq = 0;
        wchar_t* key = line;
        wchar_t* val = eq + 1;
        trim_newline(key);
        trim_newline(val);
        if(wcscmp(key, L"default_index_threads")==0){
            g_config.default_index_threads = (int)wcstol(val, NULL, 10);
        } else if(wcscmp(key, L"max_index_threads")==0){
            g_config.max_index_threads = (int)wcstol(val, NULL, 10);
        } else if(wcscmp(key, L"default_batch")==0){
            g_config.default_batch = (int)wcstol(val, NULL, 10);
        } else if(wcscmp(key, L"default_search_workers")==0){
            g_config.default_search_workers = (int)wcstol(val, NULL, 10);
        } else if(wcscmp(key, L"max_search_workers")==0){
            g_config.max_search_workers = (int)wcstol(val, NULL, 10);
        }
    }
    fclose(f);
    if(g_config.max_index_threads > MAX_THREADS) g_config.max_index_threads = MAX_THREADS;
    if(g_config.default_index_threads < 1) g_config.default_index_threads = 1;
    if(g_config.default_index_threads > g_config.max_index_threads) g_config.default_index_threads = g_config.max_index_threads;
    if(g_config.default_search_workers < 1) g_config.default_search_workers = 1;
    if(g_config.default_search_workers > g_config.max_search_workers) g_config.default_search_workers = g_config.max_search_workers;
    if(g_config.default_batch < 1) g_config.default_batch = 1;
    if(g_config.max_search_workers < 1) g_config.max_search_workers = 1;
    return TRUE;
}
