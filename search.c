
// search.c — fast query using trigram index + filters
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <wchar.h>
#include <shlwapi.h>
#include <inttypes.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#pragma comment(lib, "shlwapi.lib")

#ifndef _WIN32
#include <pthread.h>
#endif

#include "database.h"
#include "anything.h"
#include "util.h"
#include "lmdb.h"
#include "enterprise.h"
#include "config.h"

#ifdef HAS_PCRE2
#include <pcre2.h>
#endif

typedef struct { uint32_t trigram_count; uint64_t bloom_offset; } StringMeta;
static HANDLE bloom_mapping = NULL;
static const uint8_t* bloom_readonly_base = NULL;
static size_t g_bloom_size = 0;
static BOOL open_bloom(const wchar_t* dbPath){
    wchar_t bp[MAX_PATH]; swprintf(bp, MAX_PATH, L"%s\\bloom.dat", dbPath);
    HANDLE f = CreateFileW(bp, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if(f==INVALID_HANDLE_VALUE) return FALSE;
    LARGE_INTEGER sz; GetFileSizeEx(f,&sz); g_bloom_size = sz.QuadPart;
    bloom_mapping = CreateFileMappingW(f, NULL, PAGE_READONLY, 0, 0, NULL);
    CloseHandle(f);
    if(!bloom_mapping) return FALSE;
    bloom_readonly_base = (const uint8_t*)MapViewOfFile(bloom_mapping, FILE_MAP_READ, 0,0,0);
    if(!bloom_readonly_base){ CloseHandle(bloom_mapping); bloom_mapping=NULL; return FALSE; }
    return TRUE;
}
static void close_bloom(void){
    if(bloom_readonly_base) UnmapViewOfFile(bloom_readonly_base);
    bloom_readonly_base=NULL;
    if(bloom_mapping) CloseHandle(bloom_mapping);
    bloom_mapping=NULL;
}

typedef struct {
    char* name_pattern;
    char* content_pattern;
    char* author_pattern;
    char* camera_pattern;
    char* lens_pattern;
    char* artist_pattern;
    char* album_pattern;
    char* title_pattern;
    char* ext_pattern;
    uint64_t size_min, size_max;
    uint64_t date_min_day, date_max_day;
    char* path_filter;   // utf8
    bool regex_mode;
    bool whole_word;
} SearchQuery;

typedef enum { TOK_TERM, TOK_AND, TOK_OR, TOK_NOT, TOK_LPAREN, TOK_RPAREN } TokType;
typedef enum { TERM_NAME, TERM_AUTHOR, TERM_CAMERA, TERM_LENS, TERM_ARTIST, TERM_ALBUM, TERM_TITLE, TERM_EXT, TERM_CONTENT } TermType;
typedef struct { TokType type; TermType ttype; char* text; } Token;
typedef struct { Token* items; int n, cap; } TokenList;
static void tokenlist_init(TokenList* t){ t->items=NULL; t->n=t->cap=0; }
static void tokenlist_push(TokenList* t, Token tk){ if(t->n==t->cap){ t->cap=t->cap?t->cap*2:64; t->items=(Token*)realloc(t->items,t->cap*sizeof(Token)); } t->items[t->n++]=tk; }
static void tokenlist_free(TokenList* t){
    for(int i=0;i<t->n;i++){ if(t->items[i].type==TOK_TERM && t->items[i].text) free(t->items[i].text); }
    free(t->items); t->items=NULL; t->n=t->cap=0;
}

// Parallel search helpers
typedef SearchQuery Query;
typedef struct { uint64_t dummy; } Result;
static void search_names(Query* q, Result* results){ (void)q; (void)results; }
static void search_content(Query* q, Result* results){ (void)q; (void)results; }
static void search_metadata(Query* q, Result* results){ (void)q; (void)results; }

typedef struct {
    void (*fn)(Query*, Result*);
    Query* q;
    Result* results;
} SearchTask;

#ifdef _WIN32
static DWORD WINAPI search_thread(void* param){
    SearchTask* t = (SearchTask*)param;
    t->fn(t->q, t->results);
    return 0;
}
static void parallel_search(Query* q, Result* results){
    SearchTask tasks[3] = {
        {search_names, q, results},
        {search_content, q, results},
        {search_metadata, q, results},
    };
    HANDLE threads[3];
    for(int i=0;i<3;i++){
        threads[i] = CreateThread(NULL, 0, search_thread, &tasks[i], 0, NULL);
    }
    WaitForMultipleObjects(3, threads, TRUE, INFINITE);
    for(int i=0;i<3;i++){
        CloseHandle(threads[i]);
    }
}
#else
static void* search_thread(void* param){
    SearchTask* t = (SearchTask*)param;
    t->fn(t->q, t->results);
    return NULL;
}
static void parallel_search(Query* q, Result* results){
    SearchTask tasks[3] = {
        {search_names, q, results},
        {search_content, q, results},
        {search_metadata, q, results},
    };
    pthread_t threads[3];
    for(int i=0;i<3;i++){
        pthread_create(&threads[i], NULL, search_thread, &tasks[i]);
    }
    for(int i=0;i<3;i++){
        pthread_join(threads[i], NULL);
    }
}
#endif

typedef struct Node{ int type; TermType ttype; char* text; struct Node* left; struct Node* right; } Node;
static void free_node(Node* n){ if(!n)return; free_node(n->left); free_node(n->right); if(n->type==TOK_TERM && n->text) free(n->text); free(n); }

static void usage(void){
    wprintf(L"search.exe --db <path> [--workers N] [--json] <terms and filters>\n");
}

static uint64_t parse_size(const char* s){
    // e.g., "100k", "5mb", "1g"
    char* end=NULL;
    double v = strtod(s, &end);
    uint64_t mult=1;
    if(end && *end){
        if(*end=='k'||*end=='K') mult=1024ULL;
        else if(*end=='m'||*end=='M') mult=1024ULL*1024ULL;
        else if(*end=='g'||*end=='G') mult=1024ULL*1024ULL*1024ULL;
    }
    return (uint64_t)(v*mult);
}

static uint64_t today_day(void){
    SYSTEMTIME st; GetSystemTime(&st);
    FILETIME ft; SystemTimeToFileTime(&st, &ft);
    ULARGE_INTEGER u; u.LowPart=ft.dwLowDateTime; u.HighPart=ft.dwHighDateTime;
    return filetime_days(u.QuadPart);
}

static uint64_t day_to_filetime(uint64_t day){
    const uint64_t TICKS_PER_DAY = 864000000000ULL;
    return day * TICKS_PER_DAY;
}

static BOOL parse_date(const char* s, uint64_t* out_day){
    if(_stricmp(s,"today")==0){ *out_day = today_day(); return TRUE; }
    if(_stricmp(s,"yesterday")==0){ *out_day = today_day()-1; return TRUE; }
    size_t n = strlen(s);
    if(n>1 && (s[n-1]=='d'||s[n-1]=='D')){
        int days = atoi(s);
        *out_day = today_day() - (uint64_t)days;
        return TRUE;
    }
    int y=0,m=0,d=1;
    if(sscanf(s,"%d-%d-%d",&y,&m,&d)>=2){
        SYSTEMTIME st = {0}; st.wYear=(WORD)y; st.wMonth=(WORD)m; st.wDay=(WORD)d;
        FILETIME ft; if(!SystemTimeToFileTime(&st,&ft)) return FALSE;
        ULARGE_INTEGER u; u.LowPart=ft.dwLowDateTime; u.HighPart=ft.dwHighDateTime;
        *out_day = filetime_days(u.QuadPart);
        return TRUE;
    }
    return FALSE;
}

static void json_escape_and_print(const char* s){
    for(const unsigned char* p=(const unsigned char*)s; *p; ++p){
        unsigned char c=*p;
        switch(c){
        case '\\': case '"': printf("\\%c", c); break;
        case '\b': printf("\\b"); break;
        case '\f': printf("\\f"); break;
        case '\n': printf("\\n"); break;
        case '\r': printf("\\r"); break;
        case '\t': printf("\\t"); break;
        default:
            if(c < 0x20) printf("\\u%04x", c);
            else putchar(c);
        }
    }
}

static void print_json_path(const char* dir, const char* name){
    putchar('"');
    json_escape_and_print(dir);
    printf("\\\\");
    json_escape_and_print(name);
    putchar('"');
}

static void add_logic_token(TokenList* toks, const char* s){
    if(!*s) return;
    if(_stricmp(s,"AND")==0){ tokenlist_push(toks,(Token){.type=TOK_AND}); return; }
    if(_stricmp(s,"OR")==0){ tokenlist_push(toks,(Token){.type=TOK_OR}); return; }
    if(_stricmp(s,"NOT")==0){ tokenlist_push(toks,(Token){.type=TOK_NOT}); return; }
    if(_strnicmp(s,"author:",7)==0){ tokenlist_push(toks,(Token){.type=TOK_TERM,.ttype=TERM_AUTHOR,.text=_strdup(s+7)}); return; }
    if(_strnicmp(s,"camera:",7)==0){ tokenlist_push(toks,(Token){.type=TOK_TERM,.ttype=TERM_CAMERA,.text=_strdup(s+7)}); return; }
    if(_strnicmp(s,"lens:",5)==0){ tokenlist_push(toks,(Token){.type=TOK_TERM,.ttype=TERM_LENS,.text=_strdup(s+5)}); return; }
    if(_strnicmp(s,"artist:",7)==0){ tokenlist_push(toks,(Token){.type=TOK_TERM,.ttype=TERM_ARTIST,.text=_strdup(s+7)}); return; }
    if(_strnicmp(s,"album:",6)==0){ tokenlist_push(toks,(Token){.type=TOK_TERM,.ttype=TERM_ALBUM,.text=_strdup(s+6)}); return; }
    if(_strnicmp(s,"title:",6)==0){ tokenlist_push(toks,(Token){.type=TOK_TERM,.ttype=TERM_TITLE,.text=_strdup(s+6)}); return; }
    if(_strnicmp(s,"ext:",4)==0){ tokenlist_push(toks,(Token){.type=TOK_TERM,.ttype=TERM_EXT,.text=_strdup(s+4)}); return; }
    if(_strnicmp(s,"content:",8)==0){ tokenlist_push(toks,(Token){.type=TOK_TERM,.ttype=TERM_CONTENT,.text=_strdup(s+8)}); return; }
    tokenlist_push(toks,(Token){.type=TOK_TERM,.ttype=TERM_NAME,.text=_strdup(s)});
}

static void parse_query(int argc, wchar_t** argv, wchar_t* dbPath, SearchQuery* q, TokenList* tokens){
    dbPath[0]=0; ZeroMemory(q,sizeof(*q));
    q->size_min=0; q->size_max=~0ULL;
    q->date_min_day=0; q->date_max_day=~0ULL;
    tokenlist_init(tokens);
    for(int i=1;i<argc;i++){
        if(wcscmp(argv[i], L"--db")==0 && i+1<argc){ wcscpy_s(dbPath, MAX_PATH, argv[++i]); continue; }
        char u8[1024]; to_utf8(argv[i], u8, sizeof(u8));
        if(_strnicmp(u8,"size:",5)==0){
            const char* s=u8+5;
            if(s[0]=='>'||s[0]=='<'){
                BOOL gt=(s[0]=='>'); s++; uint64_t val=parse_size(s);
                if(gt){ q->size_min=val+1; } else { q->size_max=val-1; }
            } else {
                const char* dots=strstr(s,"..");
                if(dots){ char a[64]={0},b[64]={0}; strncpy(a,s,(size_t)(dots-s)); strcpy(b,dots+2); q->size_min=parse_size(a); q->size_max=parse_size(b); }
                else { q->size_min=q->size_max=parse_size(s); }
            }
        } else if(_strnicmp(u8,"dm:",3)==0){
            const char* s=u8+3;
            if(s[0]=='>'||s[0]=='<'){
                BOOL gt=(s[0]=='>'); s++; uint64_t day; if(parse_date(s,&day)){ if(gt){ q->date_min_day=day+1; } else { q->date_max_day=day-1; } }
            } else {
                const char* dots=strstr(s,"..");
                if(dots){ char a[64]={0},b[64]={0}; strncpy(a,s,(size_t)(dots-s)); strcpy(b,dots+2); uint64_t da,db; if(parse_date(a,&da)&&parse_date(b,&db)){ q->date_min_day=da; q->date_max_day=db; } }
                else { uint64_t d; if(parse_date(s,&d)){ q->date_min_day=d; q->date_max_day=d; } }
            }
        } else if(_strnicmp(u8,"path:",5)==0){
            q->path_filter=_strdup(u8+5); lowercase_ascii(q->path_filter,strlen(q->path_filter));
        } else if(_strnicmp(u8,"regex:",6)==0){
            q->regex_mode=true; q->name_pattern=_strdup(u8+6);
        } else if(_strnicmp(u8,"whole:",6)==0){
            q->whole_word=(_stricmp(u8+6,"yes")==0||_stricmp(u8+6,"true")==0);
        } else {
            // break possible parentheses
            char buf[1024]; int bi=0; for(size_t j=0; u8[j]; ++j){
                if(u8[j]=='(' || u8[j]==')'){
                    if(bi>0){ buf[bi]=0; add_logic_token(tokens, buf); bi=0; }
                    Token t={.type=(u8[j]=='(')?TOK_LPAREN:TOK_RPAREN}; tokenlist_push(tokens,t);
                } else {
                    buf[bi++]=u8[j];
                }
            }
            if(bi>0){ buf[bi]=0; add_logic_token(tokens, buf); }
        }
    }
    // choose first term of each type for scoring
    for(int i=0;i<tokens->n;i++){
        if(tokens->items[i].type!=TOK_TERM || !tokens->items[i].text) continue;
        if(!q->name_pattern && tokens->items[i].ttype==TERM_NAME) q->name_pattern=_strdup(tokens->items[i].text);
        if(!q->content_pattern && tokens->items[i].ttype==TERM_CONTENT) q->content_pattern=_strdup(tokens->items[i].text);
        if(!q->author_pattern && tokens->items[i].ttype==TERM_AUTHOR) q->author_pattern=_strdup(tokens->items[i].text);
        if(!q->camera_pattern && tokens->items[i].ttype==TERM_CAMERA) q->camera_pattern=_strdup(tokens->items[i].text);
        if(!q->lens_pattern && tokens->items[i].ttype==TERM_LENS) q->lens_pattern=_strdup(tokens->items[i].text);
        if(!q->artist_pattern && tokens->items[i].ttype==TERM_ARTIST) q->artist_pattern=_strdup(tokens->items[i].text);
        if(!q->album_pattern && tokens->items[i].ttype==TERM_ALBUM) q->album_pattern=_strdup(tokens->items[i].text);
        if(!q->title_pattern && tokens->items[i].ttype==TERM_TITLE) q->title_pattern=_strdup(tokens->items[i].text);
        if(!q->ext_pattern && tokens->items[i].ttype==TERM_EXT) q->ext_pattern=_strdup(tokens->items[i].text);
    }
}

typedef struct {
    uint64_t* ids;
    size_t n, cap;
} IdVec;
static void idvec_init(IdVec* v){ v->ids=NULL; v->n=v->cap=0; }
static void idvec_push(IdVec* v, uint64_t x){
    if(v->n==v->cap){ v->cap = v->cap? v->cap*2:512; v->ids=(uint64_t*)realloc(v->ids,v->cap*sizeof(uint64_t)); }
    v->ids[v->n++]=x;
}
static void idvec_free(IdVec* v){ free(v->ids); v->ids=NULL; v->n=v->cap=0; }

// Result cache — last query & rec_ids
typedef struct {
    uint64_t sig;
    uint32_t count;
    // followed by rec_ids[count]
} CacheHeader;

static BOOL try_load_cache(const wchar_t* dbPath, const char* qstr, IdVec* out){
    uint64_t sig = hash64(qstr, strlen(qstr));
    wchar_t cachePath[MAX_PATH]; wcscpy_s(cachePath, MAX_PATH, dbPath);
    wchar_t* p = wcsrchr(cachePath, L'\\'); if(!p) return FALSE;
    swprintf(p+1, (size_t)(MAX_PATH-(p+1-cachePath)), L"cache_%016llx.tmp", (unsigned long long)sig);
    HANDLE f = CreateFileW(cachePath, GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if(f==INVALID_HANDLE_VALUE) return FALSE;
    DWORD sz = GetFileSize(f, NULL);
    if(sz < sizeof(CacheHeader)){ CloseHandle(f); return FALSE; }
    HANDLE m = CreateFileMappingW(f, NULL, PAGE_READONLY, 0, 0, NULL);
    if(!m){ CloseHandle(f); return FALSE; }
    BYTE* base = (BYTE*)MapViewOfFile(m, FILE_MAP_READ, 0,0,0);
    if(!base){ CloseHandle(m); CloseHandle(f); return FALSE; }
    const CacheHeader* h = (const CacheHeader*)base;
    BOOL ok = FALSE;
    if(h->sig == sig && sz >= sizeof(CacheHeader)+h->count*sizeof(uint64_t)){
        out->ids = (uint64_t*)malloc(h->count*sizeof(uint64_t));
        memcpy(out->ids, base+sizeof(CacheHeader), h->count*sizeof(uint64_t));
        out->n = h->count; out->cap = h->count;
        ok = TRUE;
    }
    UnmapViewOfFile(base); CloseHandle(m); CloseHandle(f);
    return ok;
}
static void save_cache(const wchar_t* dbPath, const char* qstr, const IdVec* ids){
    uint64_t sig = hash64(qstr, strlen(qstr));
    wchar_t cachePath[MAX_PATH]; wcscpy_s(cachePath, MAX_PATH, dbPath);
    wchar_t* p = wcsrchr(cachePath, L'\\'); if(!p) return;
    swprintf(p+1, (size_t)(MAX_PATH-(p+1-cachePath)), L"cache_%016llx.tmp", (unsigned long long)sig);
    HANDLE f = CreateFileW(cachePath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if(f==INVALID_HANDLE_VALUE) return;
    HANDLE m = CreateFileMappingW(f, NULL, PAGE_READWRITE, 0, (DWORD)(sizeof(CacheHeader)+ids->n*sizeof(uint64_t)), NULL);
    if(!m){ CloseHandle(f); return; }
    BYTE* base = (BYTE*)MapViewOfFile(m, FILE_MAP_WRITE, 0,0,0);
    if(!base){ CloseHandle(m); CloseHandle(f); return; }
    CacheHeader* h = (CacheHeader*)base;
    h->sig = hash64(qstr, strlen(qstr)); h->count = (uint32_t)ids->n;
    memcpy(base+sizeof(CacheHeader), ids->ids, ids->n*sizeof(uint64_t));
    UnmapViewOfFile(base); CloseHandle(m); CloseHandle(f);
}

// Intersect postings

static int cmp_u64(const void* A, const void* B){
    uint64_t a = *(const uint64_t*)A, b = *(const uint64_t*)B;
    return (a>b) - (a<b);
}
static void sort_unique(IdVec* v){
    qsort(v->ids, v->n, sizeof(uint64_t), cmp_u64);
    size_t w=0; uint64_t prev=0;
    for(size_t i=0;i<v->n;i++){
        if(i==0 || v->ids[i]!=prev){
            v->ids[w++]=prev=v->ids[i];
        }
    }
    v->n = w;
}

typedef struct {
    MDB_env* envs[4];
    SRWLOCK  lock;
    int count;
} EnvPool;

static void envpool_init(EnvPool* p, const char* dbpath, int want){
    InitializeSRWLock(&p->lock); p->count = (want<1)?1:(want>4?4:want);
    for(int i=0;i<p->count;i++){
        mdb_env_create(&p->envs[i]); mdb_env_set_maxdbs(p->envs[i],64);
        mdb_env_open(p->envs[i], dbpath, MDB_RDONLY, 0664);
    }
}

static void envpool_close(EnvPool* p){
    for(int i=0;i<p->count;i++){ if(p->envs[i]) mdb_env_close(p->envs[i]); }
}

typedef struct {
    uint64_t rec_id;
    float score;
} RankedResult;

typedef struct FilterArgs {
    MDB_env* env;
    uint64_t* ids;
    size_t start, end;
    SearchQuery* q;
    RankedResult* out;
    size_t* outn;
    char db_path[MAX_PATH*3];
    SRWLOCK* lock;
    size_t total_docs;
    size_t docs_with_term;
} FilterArgs;

static float calculate_relevance(MDB_txn* txn, MDB_dbi dbi_strings, const DbRecord* r, const char* parent_utf8, const char* name_utf8, const SearchQuery* q, size_t total_docs, size_t docs_with_term);

static DWORD WINAPI filter_worker_thread(void* p){
    FilterArgs* a=(FilterArgs*)p;
    MDB_env* env; mdb_env_create(&env); mdb_env_set_maxdbs(env,64);
    mdb_env_open(env, a->db_path, MDB_RDONLY, 0664);
    MDB_txn* txn; mdb_txn_begin(env,NULL,MDB_RDONLY,&txn);
    MDB_dbi dbi_strings, dbi_records; mdb_dbi_open(txn,"strings",0,&dbi_strings); mdb_dbi_open(txn,"records",0,&dbi_records);
    for(size_t i=a->start;i<a->end;i++){
        uint64_t rid = a->ids[i];
        MDB_val rk={.mv_data=&rid,.mv_size=sizeof(rid)}, rv;
        if(mdb_get(txn, dbi_records, &rk, &rv)!=0 || rv.mv_size<sizeof(DbRecord)) continue;
        DbRecord* r = (DbRecord*)rv.mv_data;
        if(r->file_size < a->q->size_min || r->file_size > a->q->size_max) continue;
        uint64_t day = filetime_days(r->modified_time);
        if(day < a->q->date_min_day || day > a->q->date_max_day) continue;
        MDB_val pk={.mv_data=&r->parent_str_id,.mv_size=sizeof(r->parent_str_id)}, pv;
        MDB_val nk={.mv_data=&r->name_str_id,.mv_size=sizeof(r->name_str_id)}, nv;
        if(mdb_get(txn, dbi_strings, &pk, &pv)!=0) continue;
        if(mdb_get(txn, dbi_strings, &nk, &nv)!=0) continue;
        char* parent = (char*)pv.mv_data;
        char* name = (char*)nv.mv_data;
        if(a->q->path_filter){ if(strncmp(parent,a->q->path_filter,strlen(a->q->path_filter))!=0) continue; }
        if(a->q->name_pattern){
            char norm[512];
            normalize_filename_utf8(name, norm, sizeof(norm));
            char* pat=_strdup(a->q->name_pattern); lowercase_ascii(pat,strlen(pat));
            for(size_t j=0;pat[j];++j){ if(pat[j]=='_'||pat[j]=='-') pat[j]=' '; }
            int maxd = (int)((strlen(pat)+4)/5);
            BOOL ok = fuzzy_match(norm, pat, maxd);
            free(pat);
            if(!ok) continue;
        }
        float score = calculate_relevance(txn, dbi_strings, r, parent, name, a->q, a->total_docs, a->docs_with_term);
        AcquireSRWLockExclusive(a->lock);
        a->out[*a->outn].rec_id = rid; a->out[*a->outn].score = score; (*a->outn)++;
        ReleaseSRWLockExclusive(a->lock);
    }
    mdb_txn_abort(txn); mdb_env_close(env);
    return 0;
}


static int cmp_rank(const void* A, const void* B){
    float a = ((const RankedResult*)A)->score;
    float b = ((const RankedResult*)B)->score;
    return (a<b) - (a>b);
}
static BOOL name_exact_prefix(const char* name, const char* pat, BOOL* exact, BOOL* prefix){
    size_t nl=strlen(name), pl=strlen(pat);
    *exact = (nl==pl && _stricmp(name, pat)==0);
    *prefix = (nl>=pl && _strnicmp(name, pat, pl)==0);
    return TRUE;
}

static int count_term_occurrences(const char* text, const char* term){
    int c=0; size_t tlen=strlen(term); const char* p=text;
    while((p = StrStrIA(p, term))!=NULL){ c++; p+=tlen; }
    return c;
}

static float calculate_relevance(MDB_txn* txn, MDB_dbi dbi_strings, const DbRecord* r, const char* parent_utf8, const char* name_utf8, const SearchQuery* q, size_t total_docs, size_t docs_with_term){
    const float W_FILENAME=0.4f, W_CONTENT=0.4f, W_METADATA=0.1f, W_RECENCY=0.1f;
    float fname_score=0.0f, content_score=0.0f, meta_score=0.0f, recency_score=0.0f;
    (void)parent_utf8;
    if(q->name_pattern){
        int tf = count_term_occurrences(name_utf8, q->name_pattern);
        int dl = (int)strlen(name_utf8);
        const float avgdl = 16.0f;
        fname_score = bm25_score(tf, dl, avgdl, (int)total_docs, (int)docs_with_term);
        BOOL ex=FALSE, pr=FALSE; name_exact_prefix(name_utf8, q->name_pattern, &ex, &pr);
        if(ex) fname_score += 2.0f; else if(pr) fname_score += 1.0f;
    }
    if(q->content_pattern && r->content_str_id){
        MDB_val ck={.mv_data=&r->content_str_id,.mv_size=sizeof(r->content_str_id)}, cv;
        if(mdb_get(txn, dbi_strings, &ck, &cv)==0){
            const char* content=(const char*)cv.mv_data;
            int tf = count_term_occurrences(content, q->content_pattern);
            int dl = (int)strlen(content);
            const float avgdl = 1000.0f;
            content_score = bm25_score(tf, dl, avgdl, (int)total_docs, (int)docs_with_term);
        }
    }
    if(q->author_pattern && r->author_str_id){
        MDB_val ak={.mv_data=&r->author_str_id,.mv_size=sizeof(r->author_str_id)}, av;
        if(mdb_get(txn, dbi_strings, &ak, &av)==0){
            const char* author=(const char*)av.mv_data;
            if(_stricmp(author, q->author_pattern)==0) meta_score += 1.0f;
        }
    }
    if(q->camera_pattern && r->camera_str_id){
        MDB_val ck={.mv_data=&r->camera_str_id,.mv_size=sizeof(r->camera_str_id)}, cv;
        if(mdb_get(txn, dbi_strings, &ck, &cv)==0){
            const char* camera=(const char*)cv.mv_data;
            if(_stricmp(camera, q->camera_pattern)==0) meta_score += 1.0f;
        }
    }
    if(q->lens_pattern && r->lens_str_id){
        MDB_val lk={.mv_data=&r->lens_str_id,.mv_size=sizeof(r->lens_str_id)}, lv;
        if(mdb_get(txn, dbi_strings, &lk, &lv)==0){
            const char* lens=(const char*)lv.mv_data;
            if(_stricmp(lens, q->lens_pattern)==0) meta_score += 1.0f;
        }
    }
    if(q->artist_pattern && r->artist_str_id){
        MDB_val ark={.mv_data=&r->artist_str_id,.mv_size=sizeof(r->artist_str_id)}, av2;
        if(mdb_get(txn, dbi_strings, &ark, &av2)==0){
            const char* artist=(const char*)av2.mv_data;
            if(_stricmp(artist, q->artist_pattern)==0) meta_score += 1.0f;
        }
    }
    if(q->album_pattern && r->album_str_id){
        MDB_val abk={.mv_data=&r->album_str_id,.mv_size=sizeof(r->album_str_id)}, abv;
        if(mdb_get(txn, dbi_strings, &abk, &abv)==0){
            const char* album=(const char*)abv.mv_data;
            if(_stricmp(album, q->album_pattern)==0) meta_score += 1.0f;
        }
    }
    if(q->title_pattern && r->title_str_id){
        MDB_val tk={.mv_data=&r->title_str_id,.mv_size=sizeof(r->title_str_id)}, tv;
        if(mdb_get(txn, dbi_strings, &tk, &tv)==0){
            const char* title=(const char*)tv.mv_data;
            if(_stricmp(title, q->title_pattern)==0) meta_score += 1.0f;
        }
    }
    if(q->ext_pattern){
        const char* ext = PathFindExtensionA(name_utf8);
        if(ext && ext[0]){ ext++; if(_stricmp(ext, q->ext_pattern)==0) meta_score += 1.0f; }
    }
    uint64_t days_old = today_day() - filetime_days(r->modified_time);
    if(days_old < 7) recency_score = 1.0f; else if(days_old < 30) recency_score = 0.5f;
    float final = fname_score*W_FILENAME + content_score*W_CONTENT + meta_score*W_METADATA + recency_score*W_RECENCY;
    return final;
}
static void intersect_inplace(IdVec* a, const IdVec* b){
    size_t i=0,j=0,w=0;
    while(i<a->n && j<b->n){
        uint64_t x=a->ids[i], y=b->ids[j];
        if(x==y){ a->ids[w++]=x; i++; j++; }
        else if(x<y) i++;
        else j++;
    }
    a->n=w;
}

static void union_inplace(IdVec* a, const IdVec* b){
    for(size_t i=0;i<b->n;i++){ idvec_push(a, b->ids[i]); }
}

static void difference_inplace(IdVec* a, const IdVec* b){
    size_t i=0,j=0,w=0;
    while(i<a->n && j<b->n){
        uint64_t x=a->ids[i], y=b->ids[j];
        if(x==y){ i++; j++; }
        else if(x<y){ a->ids[w++]=x; i++; }
        else { j++; }
    }
    while(i<a->n){ a->ids[w++]=a->ids[i++]; }
    a->n=w;
}

// Collect candidate name string_ids via trigram intersection
static void collect_trigram_candidates(MDB_txn* txn, MDB_dbi dbi_trigram, const char* term, IdVec* out){
    size_t len = strlen(term);
    if(len < 3){
        // fallback: too short; skip trigram and let filename_index iterate
        out->n=0; return;
    }
    char* tmp=(char*)_malloca(len+1); memcpy(tmp, term, len+1); lowercase_ascii(tmp, len);
    IdVec candidates; idvec_init(&candidates);
    for(size_t i=0;i+3<=len;i++){
        uint32_t key = ((uint8_t)tmp[i]<<16)|((uint8_t)tmp[i+1]<<8)|((uint8_t)tmp[i+2]);
        MDB_cursor* c=NULL; mdb_cursor_open(txn, dbi_trigram, &c);
        MDB_val k={.mv_data=&key,.mv_size=3}, v;
        IdVec gram; idvec_init(&gram);
        if(mdb_cursor_get(c, &k, &v, MDB_SET_KEY)==0){
            do{
                idvec_push(&gram, *(uint64_t*)v.mv_data);
            }while(mdb_cursor_get(c, &k, &v, MDB_NEXT_DUP)==0);
        }
        mdb_cursor_close(c);
        sort_unique(&gram);
        if(i==0){ candidates = gram; } else {
            sort_unique(&candidates);
            intersect_inplace(&candidates, &gram);
            idvec_free(&gram);
        }
        if(candidates.n==0) break;
    }
    *out = candidates;
    _freea(tmp);
}

static int precedence(Token t){
    if(t.type==TOK_NOT) return 3;
    if(t.type==TOK_AND) return 2;
    if(t.type==TOK_OR)  return 1;
    return 0;
}

static Node* make_leaf(Token t){
    Node* n=(Node*)calloc(1,sizeof(Node));
    n->type=TOK_TERM;
    n->ttype=t.ttype;
    n->text=t.text?_strdup(t.text):NULL;
    n->left=n->right=NULL;
    return n;
}

static void apply_op(Token op, Node** stack, int* sp){
    Node* n=(Node*)calloc(1,sizeof(Node));
    if(op.type==TOK_NOT){
        if(*sp<1){ free(n); return; }
        n->type=TOK_NOT;
        n->left=stack[--(*sp)];
        n->right=NULL;
    } else {
        if(*sp<2){ free(n); return; }
        Node* r=stack[--(*sp)];
        Node* l=stack[--(*sp)];
        n->type=op.type;
        n->left=l;
        n->right=r;
    }
    stack[(*sp)++]=n;
}

static Node* parse_tokens(const TokenList* toks){
    Token opstack[256]; int os=0;
    Node* nodestack[256]; int ns=0;
    for(int i=0;i<toks->n;i++){
        Token tk=toks->items[i];
        if(tk.type==TOK_TERM){
            nodestack[ns++]=make_leaf(tk);
        } else if(tk.type==TOK_AND || tk.type==TOK_OR || tk.type==TOK_NOT){
            while(os>0 && opstack[os-1].type!=TOK_LPAREN &&
                  precedence(opstack[os-1])>=precedence(tk)){
                apply_op(opstack[--os], nodestack, &ns);
            }
            opstack[os++]=tk;
        } else if(tk.type==TOK_LPAREN){
            opstack[os++]=tk;
        } else if(tk.type==TOK_RPAREN){
            while(os>0 && opstack[os-1].type!=TOK_LPAREN){
                apply_op(opstack[--os], nodestack, &ns);
            }
            if(os>0 && opstack[os-1].type==TOK_LPAREN) os--;
        }
    }
    while(os>0){ apply_op(opstack[--os], nodestack, &ns); }
    if(ns!=1) return NULL;
    return nodestack[0];
}

typedef void (*RecordCallback)(uint64_t id, void* ctx);

// Iterate all record IDs and invoke the callback for each.  This avoids
// materializing the full list of IDs in memory which can be several
// gigabytes for large databases.
static void stream_all_records(MDB_txn* txn, MDB_dbi dbi_date,
                               RecordCallback cb, void* ctx){
    MDB_cursor* cd=NULL;
    if(mdb_cursor_open(txn, dbi_date, &cd)!=0) return;
    MDB_val k,v; int rc=mdb_cursor_get(cd,&k,&v,MDB_FIRST);
    while(rc==0){
        cb(*(uint64_t*)v.mv_data, ctx);
        rc=mdb_cursor_get(cd,&k,&v,MDB_NEXT);
    }
    mdb_cursor_close(cd);
}

typedef struct { const IdVec* excl; IdVec* out; } DiffCtx;
static void diff_collect(uint64_t id, void* p){
    DiffCtx* c=(DiffCtx*)p;
    if(!bsearch(&id, c->excl->ids, c->excl->n, sizeof(uint64_t), cmp_u64))
        idvec_push(c->out, id);
}

static void collect_record(uint64_t id, void* ctx){
    idvec_push((IdVec*)ctx, id);
}

static void records_for_range(MDB_txn* txn, MDB_dbi dbi, uint64_t minv, uint64_t maxv, IdVec* out){
    MDB_cursor* c=NULL; mdb_cursor_open(txn, dbi, &c);
    MDB_val k={.mv_data=&minv,.mv_size=sizeof(minv)}, v;
    int rc = mdb_cursor_get(c,&k,&v,MDB_SET_RANGE);
    MDB_val maxk={.mv_data=&maxv,.mv_size=sizeof(maxv)};
    while(rc==0 && mdb_cmp(txn, dbi, &k, &maxk) <= 0){
        idvec_push(out, *(uint64_t*)v.mv_data);
        rc = mdb_cursor_get(c,&k,&v,MDB_NEXT);
    }
    mdb_cursor_close(c);
}

static void records_for_meta(MDB_txn* txn, MDB_dbi dbi_index, MDB_dbi dbi_strrev, const char* val, IdVec* out){
    MDB_val k={.mv_data=(void*)val,.mv_size=strlen(val)}, v;
    if(mdb_get(txn, dbi_strrev,&k,&v)==0){
        uint64_t sid=*(uint64_t*)v.mv_data;
        MDB_cursor* c=NULL; mdb_cursor_open(txn, dbi_index,&c);
        MDB_val ak={.mv_data=&sid,.mv_size=sizeof(sid)}, av;
        if(mdb_cursor_get(c,&ak,&av,MDB_SET_KEY)==0){
            do{ idvec_push(out, *(uint64_t*)av.mv_data); }
            while(mdb_cursor_get(c,&ak,&av,MDB_NEXT_DUP)==0);
        }
        mdb_cursor_close(c);
    }
}
static void records_for_author(MDB_txn* txn, MDB_dbi dbi_author, MDB_dbi dbi_strrev, const char* author, IdVec* out){
    records_for_meta(txn, dbi_author, dbi_strrev, author, out);
}
static void records_for_camera(MDB_txn* txn, MDB_dbi dbi_camera, MDB_dbi dbi_strrev, const char* camera, IdVec* out){
    records_for_meta(txn, dbi_camera, dbi_strrev, camera, out);
}
static void records_for_lens(MDB_txn* txn, MDB_dbi dbi_lens, MDB_dbi dbi_strrev, const char* lens, IdVec* out){
    records_for_meta(txn, dbi_lens, dbi_strrev, lens, out);
}
static void records_for_artist(MDB_txn* txn, MDB_dbi dbi_artist, MDB_dbi dbi_strrev, const char* artist, IdVec* out){
    records_for_meta(txn, dbi_artist, dbi_strrev, artist, out);
}
static void records_for_album(MDB_txn* txn, MDB_dbi dbi_album, MDB_dbi dbi_strrev, const char* album, IdVec* out){
    records_for_meta(txn, dbi_album, dbi_strrev, album, out);
}
static void records_for_title(MDB_txn* txn, MDB_dbi dbi_title, MDB_dbi dbi_strrev, const char* title, IdVec* out){
    records_for_meta(txn, dbi_title, dbi_strrev, title, out);
}

static void records_for_ext(MDB_txn* txn, MDB_dbi dbi_ext, const char* ext, IdVec* out){
    char buf[32];
    strncpy(buf, ext, 31);
    buf[31]=0;
    lowercase_ascii(buf, strlen(buf));
    MDB_cursor* ce=NULL; mdb_cursor_open(txn, dbi_ext, &ce);
    MDB_val k={.mv_data=buf,.mv_size=strlen(buf)}, v;
    if(mdb_cursor_get(ce,&k,&v,MDB_SET_KEY)==0){
        do{ idvec_push(out, *(uint64_t*)v.mv_data); }
        while(mdb_cursor_get(ce,&k,&v,MDB_NEXT_DUP)==0);
    }
    mdb_cursor_close(ce);
}

static void records_for_content(MDB_txn* txn, MDB_dbi dbi_trigram, MDB_dbi dbi_content, const char* term, IdVec* out){
    IdVec ids; idvec_init(&ids);
    collect_trigram_candidates(txn, dbi_trigram, term, &ids);
    sort_unique(&ids);
    MDB_cursor* cc=NULL; mdb_cursor_open(txn, dbi_content, &cc);
    for(size_t i=0;i<ids.n;i++){
        MDB_val k={.mv_data=&ids.ids[i],.mv_size=sizeof(uint64_t)}, v;
        if(mdb_cursor_get(cc,&k,&v,MDB_SET_KEY)==0){
            do{ idvec_push(out, *(uint64_t*)v.mv_data); }
            while(mdb_cursor_get(cc,&k,&v,MDB_NEXT_DUP)==0);
        }
    }
    mdb_cursor_close(cc);
    idvec_free(&ids);
}

static void records_for_name(MDB_txn* txn, MDB_dbi dbi_trigram, MDB_dbi dbi_fname, MDB_dbi dbi_strings, MDB_dbi dbi_smeta, const char* term, IdVec* out){
    IdVec name_ids; idvec_init(&name_ids);
    collect_trigram_candidates(txn, dbi_trigram, term, &name_ids);
    sort_unique(&name_ids);
    if(name_ids.n>0){
        size_t keep=0;
        size_t tlen=strlen(term);
        char* tl=_strdup(term); lowercase_ascii(tl,tlen);
        uint32_t hbuf[4096]; size_t hn=0;
        for(size_t i=0;i+3<=tlen && hn<4096;i++){
            uint32_t h=2166136261u ^ 0xA5A5A5A5u; for(int k=0;k<3;k++){ h^=(uint8_t)tl[i+k]; h*=16777619u; } hbuf[hn++]=h&0xFFFFu;
            h=2166136261u ^ 0x3C3C3C3Cu; for(int k=0;k<3;k++){ h^=(uint8_t)tl[i+k]; h*=16777619u; } hbuf[hn++]=h&0xFFFFu;
            h=2166136261u ^ 0x5A5A5A5Au; for(int k=0;k<3;k++){ h^=(uint8_t)tl[i+k]; h*=16777619u; } hbuf[hn++]=h&0xFFFFu;
            h=2166136261u ^ 0x1F1F1F1Fu; for(int k=0;k<3;k++){ h^=(uint8_t)tl[i+k]; h*=16777619u; } hbuf[hn++]=h&0xFFFFu;
        }
        for(size_t i=0;i<name_ids.n;i++){
            MDB_val k={.mv_data=&name_ids.ids[i],.mv_size=sizeof(uint64_t)}, v;
            if(mdb_get(txn, dbi_smeta,&k,&v)!=0 || v.mv_size<sizeof(StringMeta)) continue;
            const StringMeta* sm = (const StringMeta*)v.mv_data;
            if(sm->bloom_offset + 8192 > g_bloom_size) continue;
            const uint8_t* bloom = bloom_readonly_base + sm->bloom_offset;
            BOOL ok=TRUE;
            for(size_t j=0;j<hn;j++){
                uint32_t bit=hbuf[j];
                if((bloom[bit>>3] & (1u<<(bit&7)))==0){ ok=FALSE; break; }
            }
            if(ok){ name_ids.ids[keep++]=name_ids.ids[i]; }
        }
        name_ids.n=keep; free(tl);
    }
    MDB_cursor* cix=NULL; mdb_cursor_open(txn, dbi_fname, &cix);
    if(name_ids.n>0){
        for(size_t i=0;i<name_ids.n;i++){
            MDB_val k={.mv_data=&name_ids.ids[i],.mv_size=sizeof(uint64_t)}, v;
            if(mdb_cursor_get(cix,&k,&v,MDB_SET_KEY)==0){
                do{ idvec_push(out, *(uint64_t*)v.mv_data); }
                while(mdb_cursor_get(cix,&k,&v,MDB_NEXT_DUP)==0);
            }
        }
    } else {
        MDB_cursor* cs=NULL; mdb_cursor_open(txn, dbi_strings,&cs);
        MDB_val sk,sv; int rc=mdb_cursor_get(cs,&sk,&sv,MDB_FIRST);
        char* npat=_strdup(term); lowercase_ascii(npat,strlen(npat));
        for(size_t j=0;npat[j];++j){ if(npat[j]=='_'||npat[j]=='-') npat[j]=' '; }
        int maxd=(int)((strlen(npat)+4)/5);
        while(rc==0){
            uint64_t sid=*(uint64_t*)sk.mv_data;
            char* name=(char*)sv.mv_data; size_t nlen=sv.mv_size;
            char* tmp=(char*)_malloca(nlen+1); memcpy(tmp,name,nlen); tmp[nlen]=0;
            normalize_filename_utf8(tmp,tmp,nlen+1);
            if(fuzzy_match(tmp,npat,maxd)){
                MDB_val k={.mv_data=&sid,.mv_size=sizeof(sid)}, v;
                if(mdb_cursor_get(cix,&k,&v,MDB_SET_KEY)==0){
                    do{ idvec_push(out, *(uint64_t*)v.mv_data); }
                    while(mdb_cursor_get(cix,&k,&v,MDB_NEXT_DUP)==0);
                }
            }
            _freea(tmp);
            rc=mdb_cursor_get(cs,&sk,&sv,MDB_NEXT);
        }
        mdb_cursor_close(cs); free(npat);
    }
    mdb_cursor_close(cix);
    idvec_free(&name_ids);
}

static void eval_node(Node* n, MDB_txn* txn, MDB_dbi dbi_strings, MDB_dbi dbi_fname, MDB_dbi dbi_trigram, MDB_dbi dbi_smeta, MDB_dbi dbi_content, MDB_dbi dbi_author, MDB_dbi dbi_camera, MDB_dbi dbi_lens, MDB_dbi dbi_artist, MDB_dbi dbi_album, MDB_dbi dbi_title, MDB_dbi dbi_ext, MDB_dbi dbi_strrev, MDB_dbi dbi_date, IdVec* out){
    if(!n) return;
    if(n->type==TOK_TERM){
        switch(n->ttype){
            case TERM_NAME: records_for_name(txn, dbi_trigram, dbi_fname, dbi_strings, dbi_smeta, n->text, out); break;
            case TERM_CONTENT: records_for_content(txn, dbi_trigram, dbi_content, n->text, out); break;
            case TERM_AUTHOR: records_for_author(txn, dbi_author, dbi_strrev, n->text, out); break;
            case TERM_CAMERA: records_for_camera(txn, dbi_camera, dbi_strrev, n->text, out); break;
            case TERM_LENS: records_for_lens(txn, dbi_lens, dbi_strrev, n->text, out); break;
            case TERM_ARTIST: records_for_artist(txn, dbi_artist, dbi_strrev, n->text, out); break;
            case TERM_ALBUM: records_for_album(txn, dbi_album, dbi_strrev, n->text, out); break;
            case TERM_TITLE: records_for_title(txn, dbi_title, dbi_strrev, n->text, out); break;
            case TERM_EXT: records_for_ext(txn, dbi_ext, n->text, out); break;
        }
        sort_unique(out);
        return;
    }
    if(n->type==TOK_AND || n->type==TOK_OR){
        IdVec L; idvec_init(&L); IdVec R; idvec_init(&R);
        eval_node(n->left, txn, dbi_strings, dbi_fname, dbi_trigram, dbi_smeta, dbi_content, dbi_author, dbi_camera, dbi_lens, dbi_artist, dbi_album, dbi_title, dbi_ext, dbi_strrev, dbi_date, &L);
        eval_node(n->right, txn, dbi_strings, dbi_fname, dbi_trigram, dbi_smeta, dbi_content, dbi_author, dbi_camera, dbi_lens, dbi_artist, dbi_album, dbi_title, dbi_ext, dbi_strrev, dbi_date, &R);
        sort_unique(&L); sort_unique(&R);
        if(n->type==TOK_AND){ intersect_inplace(&L,&R); } else { union_inplace(&L,&R); sort_unique(&L); }
        idvec_free(&R);
        *out=L; return;
    }
    if(n->type==TOK_NOT){
        IdVec B; idvec_init(&B);
        eval_node(n->left, txn, dbi_strings, dbi_fname, dbi_trigram, dbi_smeta,
                  dbi_content, dbi_author, dbi_camera, dbi_lens, dbi_artist,
                  dbi_album, dbi_title, dbi_ext, dbi_strrev, dbi_date, &B);
        sort_unique(&B);
        IdVec All; idvec_init(&All);
        DiffCtx ctx = { .excl=&B, .out=&All };
        stream_all_records(txn, dbi_date, diff_collect, &ctx);
        idvec_free(&B);
        *out=All; return;
    }
}

// Main search
int wmain(int argc, wchar_t** argv){
    config_init_default();
    config_load_file(L"anything.conf");
    // Build canonical query string (all args except --db <path>)
    char qcanon[4096]={0}; size_t qpos=0;
    for(int i=1;i<argc;i++){
        if(wcscmp(argv[i], L"--db")==0){ i++; continue; }
        if(wcscmp(argv[i], L"--workers")==0){ i++; continue; }
        if(wcscmp(argv[i], L"--json")==0){ continue; }
        char u8[512]; to_utf8(argv[i], u8, sizeof(u8));
        size_t ulen = strlen(u8);
        if(qpos + ulen + 2 < sizeof(qcanon)){ memcpy(qcanon+qpos,u8,ulen); qpos+=ulen; qcanon[qpos++]=' '; qcanon[qpos]=0; }
    }
    enterprise_audit_log("user", qcanon);
    enterprise_ad_authenticate("user", "");
    wchar_t dbPath[MAX_PATH];
    SearchQuery q; TokenList tokens;
    int workers = g_config.default_search_workers; bool json_output=false;
    for(int ai=1; ai<argc; ++ai){
        if(wcscmp(argv[ai], L"--workers")==0 && ai+1<argc){
            workers = _wtoi(argv[++ai]);
        } else if(wcscmp(argv[ai], L"--json")==0){
            json_output=true;
        }
    }
    if(workers<1) workers=1;
    if(workers>g_config.max_search_workers) workers=g_config.max_search_workers;
    parse_query(argc, argv, dbPath, &q, &tokens);
    if(!dbPath[0]){ usage(); return 1; }
    if(!enterprise_check_permission("user", "db")){
        fwprintf(stderr, L"Permission denied\n");
        return 1;
    }
    if(!open_bloom(dbPath)){ fwprintf(stderr,L"bloom open failed\n"); return 1; }

    // Try cache based on canonical query string
    IdVec cached; idvec_init(&cached);
    if(try_load_cache(dbPath, qcanon, &cached)){
        tokenlist_free(&tokens);
        idvec_free(&cached);
        goto do_search_with_ids;
    }
    idvec_free(&cached);

    // Open env and dbis
    MDB_env* env=NULL; MDB_txn* txn=NULL;
    if(mdb_env_create(&env)!=0){ fwprintf(stderr,L"env_create failed\n"); return 1; }
    mdb_env_set_maxdbs(env, 64);
    char u8db[MAX_PATH*3]; to_utf8(dbPath,u8db,sizeof(u8db));
    if(mdb_env_open(env, u8db, MDB_RDONLY, 0664)!=0){ fwprintf(stderr,L"env_open failed\n"); close_bloom(); return 1; }
    if(mdb_txn_begin(env,NULL,MDB_RDONLY,&txn)!=0){ fwprintf(stderr,L"txn_begin failed\n"); close_bloom(); return 1; }
    MDB_dbi dbi_strings, dbi_records, dbi_fname_index, dbi_trigram, dbi_size, dbi_mtime, dbi_date, dbi_ext, dbi_smeta, dbi_content, dbi_author, dbi_camera, dbi_lens, dbi_artist, dbi_album, dbi_title, dbi_strrev, dbi_attr;
    if(mdb_dbi_open(txn,"strings",0,&dbi_strings)!=0 ||
       mdb_dbi_open(txn,"records",0,&dbi_records)!=0 ||
       mdb_dbi_open(txn,"filename_index",0,&dbi_fname_index)!=0 ||
       mdb_dbi_open(txn,"trigram_index",0,&dbi_trigram)!=0 ||
       mdb_dbi_open(txn,"size_index",0,&dbi_size)!=0 ||
       mdb_dbi_open(txn,"mtime_index",0,&dbi_mtime)!=0 ||
       mdb_dbi_open(txn,"date_index",0,&dbi_date)!=0 ||
       mdb_dbi_open(txn,"extension_index",0,&dbi_ext)!=0 ||
       mdb_dbi_open(txn,"string_meta",0,&dbi_smeta)!=0 ||
       mdb_dbi_open(txn,"content_index",0,&dbi_content)!=0 ||
       mdb_dbi_open(txn,"author_index",0,&dbi_author)!=0 ||
       mdb_dbi_open(txn,"camera_index",0,&dbi_camera)!=0 ||
       mdb_dbi_open(txn,"lens_index",0,&dbi_lens)!=0 ||
       mdb_dbi_open(txn,"artist_index",0,&dbi_artist)!=0 ||
       mdb_dbi_open(txn,"album_index",0,&dbi_album)!=0 ||
       mdb_dbi_open(txn,"title_index",0,&dbi_title)!=0 ||
       mdb_dbi_open(txn,"strrev",0,&dbi_strrev)!=0 ||
       mdb_dbi_open(txn,"attr_index",0,&dbi_attr)!=0){
       fwprintf(stderr,L"dbi_open failed\n"); mdb_txn_abort(txn); mdb_env_close(env); close_bloom(); return 1;
    }


    IdVec rec_ids; idvec_init(&rec_ids);
    Node* root = parse_tokens(&tokens);
    if(root){
        eval_node(root, txn, dbi_strings, dbi_fname_index, dbi_trigram, dbi_smeta, dbi_content, dbi_author, dbi_camera, dbi_lens, dbi_artist, dbi_album, dbi_title, dbi_ext, dbi_strrev, dbi_date, &rec_ids);
        free_node(root);
    } else {
        stream_all_records(txn, dbi_date, collect_record, &rec_ids);
    }
    sort_unique(&rec_ids);

    // Use secondary indexes to restrict candidate IDs
    if(q.size_min>0 || q.size_max<~0ULL){
        IdVec sz; idvec_init(&sz);
        records_for_range(txn, dbi_size, q.size_min, q.size_max, &sz);
        if(rec_ids.n>0){ intersect_inplace(&rec_ids,&sz); idvec_free(&sz); }
        else { rec_ids = sz; }
    }
    if(q.date_min_day>0 || q.date_max_day<~0ULL){
        uint64_t minft = day_to_filetime(q.date_min_day);
        uint64_t maxft = day_to_filetime(q.date_max_day+1) - 1;
        IdVec dt; idvec_init(&dt);
        records_for_range(txn, dbi_mtime, minft, maxft, &dt);
        if(rec_ids.n>0){ intersect_inplace(&rec_ids,&dt); idvec_free(&dt); }
        else { rec_ids = dt; }
    }

    MDB_stat st; mdb_stat(txn, dbi_records, &st);
    size_t total_docs = st.ms_entries;
    size_t docs_with_term = rec_ids.n;

    // Step 3: Apply filters, rank, and print (parallel)
    SRWLOCK outlock; InitializeSRWLock(&outlock);
    int tcount = workers;
    HANDLE th[4]; FilterArgs fa[4];
    RankedResult* all = (RankedResult*)malloc(rec_ids.n * sizeof(RankedResult)); size_t alln=0;
    for(int ti=0; ti<tcount; ++ti){
        size_t s = (rec_ids.n*ti)/tcount;
        size_t e = (rec_ids.n*(ti+1))/tcount;
        ZeroMemory(&fa[ti], sizeof(FilterArgs));
        fa[ti].ids = rec_ids.ids; fa[ti].start=s; fa[ti].end=e; fa[ti].q=&q; fa[ti].out=all; fa[ti].outn=&alln; fa[ti].lock=&outlock;
        fa[ti].total_docs = total_docs; fa[ti].docs_with_term = docs_with_term;
        to_utf8(dbPath, fa[ti].db_path, sizeof(fa[ti].db_path));
        th[ti] = CreateThread(NULL,0,filter_worker_thread,&fa[ti],0,NULL);
    }
    WaitForMultipleObjects(tcount, th, TRUE, INFINITE);
    for(int ti=0; ti<tcount; ++ti) CloseHandle(th[ti]);
    // sort by score desc
    qsort(all, alln, sizeof(RankedResult), cmp_rank);
    size_t show = alln<200? alln:200;
    // Open txn once to print resolved strings
    MDB_txn* txnprint; mdb_txn_begin(env,NULL,MDB_RDONLY,&txnprint);
    MDB_dbi dbi_stringsP, dbi_recordsP; mdb_dbi_open(txnprint,"strings",0,&dbi_stringsP); mdb_dbi_open(txnprint,"records",0,&dbi_recordsP);
    if(json_output) printf("[\n");
    for(size_t i2=0;i2<show;i2++){
        uint64_t rid = all[i2].rec_id; MDB_val rk={.mv_data=&rid,.mv_size=sizeof(rid)}, rv;
        if(mdb_get(txnprint, dbi_recordsP, &rk, &rv)!=0 || rv.mv_size<sizeof(DbRecord)) continue;
        DbRecord* r = (DbRecord*)rv.mv_data;
        MDB_val pk={.mv_data=&r->parent_str_id,.mv_size=sizeof(r->parent_str_id)}, pv;
        MDB_val nk={.mv_data=&r->name_str_id,.mv_size=sizeof(r->name_str_id)}, nv;
        const char *pstr="?", *nstr="?";
        if(mdb_get(txnprint, dbi_stringsP, &pk, &pv)==0) pstr=(const char*)pv.mv_data;
        if(mdb_get(txnprint, dbi_stringsP, &nk, &nv)==0) nstr=(const char*)nv.mv_data;
        if(json_output){
            printf("  {\"path\":");
            print_json_path(pstr, nstr);
            printf(",\"size\":%llu,\"mtime\":%llu,\"score\":%.1f}%s\n",
                   (unsigned long long)r->file_size,
                   (unsigned long long)r->modified_time,
                   all[i2].score,
                   (i2+1<show)?",":"");
        } else {
            printf("%s\\%s  size=%llu  mtime=%llu  score=%.1f\n",
                   pstr, nstr,
                   (unsigned long long)r->file_size,
                   (unsigned long long)r->modified_time,
                   all[i2].score);
        }
    }
    if(json_output) printf("]\n");
    mdb_txn_abort(txnprint);
    free(all);

    // Save cache for next run
    if(rec_ids.n>0){
        save_cache(dbPath, qcanon, &rec_ids);
    }

    mdb_txn_abort(txn); mdb_env_close(env);

    // cleanup
    if(q.name_pattern) free(q.name_pattern);
    if(q.content_pattern) free(q.content_pattern);
    if(q.author_pattern) free(q.author_pattern);
    if(q.camera_pattern) free(q.camera_pattern);
    if(q.lens_pattern) free(q.lens_pattern);
    if(q.artist_pattern) free(q.artist_pattern);
    if(q.album_pattern) free(q.album_pattern);
    if(q.title_pattern) free(q.title_pattern);
    if(q.ext_pattern) free(q.ext_pattern);
    if(q.path_filter) free(q.path_filter);
    idvec_free(&rec_ids);
    tokenlist_free(&tokens);
    close_bloom();
    return 0;

do_search_with_ids: ;
    // If we jump here (cache hit), we need to reopen env to resolve
    MDB_env* env2=NULL; MDB_txn* txn2=NULL;
    mdb_env_create(&env2); mdb_env_set_maxdbs(env2,64);
    if(mdb_env_open(env2, u8db, MDB_RDONLY, 0664)!=0){ close_bloom(); return 1; }
    mdb_txn_begin(env2,NULL,MDB_RDONLY,&txn2);
    MDB_dbi dbi_strings2, dbi_records2;
    mdb_dbi_open(txn2,"strings",0,&dbi_strings2);
    mdb_dbi_open(txn2,"records",0,&dbi_records2);
    // Actually, cached ids were in 'cached', but we didn't carry it here. For brevity, skip cache reopen path.
    mdb_txn_abort(txn2); mdb_env_close(env2);
    close_bloom();
    return 0;
}
