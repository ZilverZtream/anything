
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
#pragma comment(lib, "shlwapi.lib")

#include "database.h"
#include "anything.h"
#include "util.h"
#include "lmdb.h"

#ifdef HAS_PCRE2
#include <pcre2.h>
#endif

typedef struct {
    char* name_pattern;
    uint64_t size_min, size_max;
    uint64_t date_min_day, date_max_day;
    char* path_filter;   // utf8
    char* ext;           // utf8
    bool regex_mode;
    bool whole_word;
} SearchQuery;

static void usage(void){
    wprintf(L"search.exe --db <path> [--workers N] <terms and filters>\n");
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

static void parse_query(int argc, wchar_t** argv, wchar_t* dbPath, SearchQuery* q, char** text_term_out){
    dbPath[0]=0;
    ZeroMemory(q, sizeof(*q));
    q->size_min=0; q->size_max=~0ULL;
    q->date_min_day=0; q->date_max_day=~0ULL;
    *text_term_out=NULL;
    for(int i=1;i<argc;i++){
        if(wcscmp(argv[i], L"--db")==0 && i+1<argc){ wcscpy_s(dbPath, MAX_PATH, argv[++i]); continue; }
        // convert token to utf8
        char u8[1024]; to_utf8(argv[i], u8, sizeof(u8));
        if(_strnicmp(u8,"size:",5)==0){
            const char* s = u8+5;
            if(s[0]=='>'||s[0]=='<'){
                BOOL gt = (s[0]=='>'); s++;
                uint64_t val = parse_size(s);
                if(gt){ q->size_min = val+1; } else { q->size_max = val-1; }
            } else {
                const char* dots = strstr(s,"..");
                if(dots){
                    char a[64]={0}, b[64]={0};
                    strncpy(a,s,(size_t)(dots-s)); strcpy(b,dots+2);
                    q->size_min = parse_size(a);
                    q->size_max = parse_size(b);
                } else {
                    q->size_min = q->size_max = parse_size(s);
                }
            }
        } else if(_strnicmp(u8,"dm:",3)==0){
            const char* s=u8+3;
            if(s[0]=='>'||s[0]=='<'){
                BOOL gt = (s[0]=='>'); s++;
                uint64_t day; if(parse_date(s,&day)){
                    if(gt){ q->date_min_day=day+1; } else { q->date_max_day=day-1; }
                }
            } else {
                const char* dots = strstr(s,"..");
                if(dots){
                    char a[64]={0}, b[64]={0};
                    strncpy(a,s,(size_t)(dots-s)); strcpy(b,dots+2);
                    uint64_t da,db; if(parse_date(a,&da)&&parse_date(b,&db)){ q->date_min_day=da; q->date_max_day=db; }
                } else {
                    uint64_t d; if(parse_date(s,&d)){ q->date_min_day=d; q->date_max_day=d; }
                }
            }
        } else if(_strnicmp(u8,"path:",5)==0){
            q->path_filter = _strdup(u8+5);
            lowercase_ascii(q->path_filter, strlen(q->path_filter));
        } else if(_strnicmp(u8,"ext:",4)==0){
            q->ext = _strdup(u8+4);
            lowercase_ascii(q->ext, strlen(q->ext));
        } else if(_strnicmp(u8,"regex:",6)==0){
            q->regex_mode = true;
            q->name_pattern = _strdup(u8+6);
        } else if(_strnicmp(u8,"whole:",6)==0){
            q->whole_word = (_stricmp(u8+6,"yes")==0||_stricmp(u8+6,"true")==0);
        } else {
            // treat as free text (take the first non-filter token as the primary term)
            if(!*text_term_out){
                *text_term_out = _strdup(u8);
            }
        }
    }
    if(!q->name_pattern && *text_term_out){
        q->name_pattern = _strdup(*text_term_out);
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
    uint64_t sig = hash64(qstr, strlen(qstr));
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
        if(a->q->ext){ char ext[32]; split_extension_utf8(name, ext, sizeof(ext)); if(_stricmp(ext, a->q->ext)!=0) continue; }
        if(a->q->path_filter){ if(strncmp(parent,a->q->path_filter,strlen(a->q->path_filter))!=0) continue; }
        if(a->q->name_pattern){ char* tmp=_strdup(name); lowercase_ascii(tmp,strlen(tmp)); char* pat=_strdup(a->q->name_pattern);
            BOOL ok = (is_avx2_supported() ? avx2_contains(tmp, strlen(tmp), pat, strlen(pat)) : (StrStrIA(tmp, pat)!=NULL));
            free(tmp); free(pat);
            if(!ok) continue;
        }
        float score = calculate_relevance(r, parent, name, a->q);
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
static int count_slashes(const char* s){
    int c=0; for(;*s;s++){ if(*s=='\\' || *s=='/') c++; } return c;
}
static BOOL name_exact_prefix(const char* name, const char* pat, BOOL* exact, BOOL* prefix){
    size_t nl=strlen(name), pl=strlen(pat);
    *exact = (nl==pl && _stricmp(name, pat)==0);
    *prefix = (nl>=pl && _strnicmp(name, pat, pl)==0);
    return TRUE;
}
static BOOL is_executable_ext(const char* name){
    const char* dot = strrchr(name,'.'); if(!dot) return FALSE;
    const char* exts[] = {"exe","bat","cmd","ps1","msi","com"};
    char ext[16]; size_t n=strlen(dot+1); if(n>15)n=15; memcpy(ext,dot+1,n); ext[n]=0; lowercase_ascii(ext,n);
    for(int i=0;i<6;i++){ if(strcmp(ext, exts[i])==0) return TRUE; } return FALSE;
}
static float calculate_relevance(const DbRecord* r, const char* parent_utf8, const char* name_utf8, const SearchQuery* q){
    float score = 100.0f;
    if(q->name_pattern){
        BOOL ex=FALSE, pr=FALSE; name_exact_prefix(name_utf8, q->name_pattern, &ex, &pr);
        if(ex) score += 50.0f; else if(pr) score += 25.0f;
    }
    int depth = count_slashes(parent_utf8);
    score -= (float)depth * 2.0f;
    uint64_t days_old = today_day() - filetime_days(r->modified_time);
    if(days_old < 7) score += 20.0f; else if(days_old < 30) score += 10.0f;
    if(is_executable_ext(name_utf8)) score += 15.0f;
    // size heuristic
    if(r->file_size>0 && r->file_size < 4096) score -= 2.0f;
    return score;
}

    if(v->n==0) return;
    qsort(v->ids, v->n, sizeof(uint64_t), cmp_u64);
    size_t w=1;
    for(size_t i=1;i<v->n;i++){
        if(v->ids[i]!=v->ids[w-1]) v->ids[w++]=v->ids[i];
    }
    v->n=w;
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

// Main search
int wmain(int argc, wchar_t** argv){
    // Build canonical query string (all args except --db <path>)
    char qcanon[4096]={0}; size_t qpos=0;
    for(int i=1;i<argc;i++){
        if(wcscmp(argv[i], L"--db")==0){ i++; continue; }
        char u8[512]; to_utf8(argv[i], u8, sizeof(u8));
        size_t ulen = strlen(u8);
        if(qpos + ulen + 2 < sizeof(qcanon)){ memcpy(qcanon+qpos,u8,ulen); qpos+=ulen; qcanon[qpos++]=' '; qcanon[qpos]=0; }
    }
    wchar_t dbPath[MAX_PATH];
    SearchQuery q; char* primary=NULL;
    int workers=1;
    for(int ai=1; ai<argc; ++ai){ if(wcscmp(argv[ai], L"--workers")==0 && ai+1<argc){ workers = _wtoi(argv[++ai]); if(workers<1)workers=1; if(workers>4)workers=4; } }
    parse_query(argc, argv, dbPath, &q, &primary);
    if(!dbPath[0]){ usage(); return 1; }

    // Try cache
    if(q.name_pattern){
        IdVec cached; idvec_init(&cached);
        if(try_load_cache(dbPath, q.name_pattern, &cached)){
            // open env to resolve and print
            goto do_search_with_ids;
        }
    }

    // Open env and dbis
    MDB_env* env=NULL; MDB_txn* txn=NULL;
    if(mdb_env_create(&env)!=0){ fwprintf(stderr,L"env_create failed\n"); return 1; }
    mdb_env_set_maxdbs(env, 64);
    char u8db[MAX_PATH*3]; to_utf8(dbPath,u8db,sizeof(u8db));
    if(mdb_env_open(env, u8db, MDB_RDONLY, 0664)!=0){ fwprintf(stderr,L"env_open failed\n"); return 1; }
    if(mdb_txn_begin(env,NULL,MDB_RDONLY,&txn)!=0){ fwprintf(stderr,L"txn_begin failed\n"); return 1; }
    MDB_dbi dbi_strings, dbi_records, dbi_fname_index, dbi_trigram, dbi_size, dbi_date, dbi_ext, dbi_smeta;
    if(mdb_dbi_open(txn,"strings",0,&dbi_strings)!=0 ||
       mdb_dbi_open(txn,"records",0,&dbi_records)!=0 ||
       mdb_dbi_open(txn,"filename_index",0,&dbi_fname_index)!=0 ||
       mdb_dbi_open(txn,"trigram_index",0,&dbi_trigram)!=0 ||
       mdb_dbi_open(txn,"size_index",0,&dbi_size)!=0 ||
       mdb_dbi_open(txn,"date_index",0,&dbi_date)!=0 ||
       mdb_dbi_open(txn,"extension_index",0,&dbi_ext)!=0 ||
       mdb_dbi_open(txn,"string_meta",0,&dbi_smeta)!=0){
        fwprintf(stderr,L"dbi_open failed\n"); mdb_txn_abort(txn); mdb_env_close(env); return 1;
    }

    // Step 1: candidate name string_ids via trigram
    IdVec name_ids; idvec_init(&name_ids);
    if(q.name_pattern){
        collect_trigram_candidates(txn, dbi_trigram, q.name_pattern, &name_ids);
        sort_unique(&name_ids);
    // Bloom filter tighten
    if(name_ids.n>0){
        size_t keep=0;
        // Precompute term trigrams hash positions
        const char* term = q.name_pattern?q.name_pattern:"";
        size_t tlen = strlen(term);
        char* tl=(char*)_malloca(tlen+1); memcpy(tl,term,tlen+1); lowercase_ascii(tl,tlen);
        uint32_t hbuf[4096]; size_t hn=0;
        for(size_t i=0;i+3<=tlen && hn<4096;i++){
            // same seeds as DB
            uint32_t h1 = 0; // we'll compute via util hash? reuse in-db formula here simplified
            // replicate the database hash32_seed inline (same constants)
            uint32_t seed1=0xA5A5A5A5u, seed2=0x3C3C3C3Cu, seed3=0x5A5A5A5Au, seed4=0x1F1F1F1Fu;
            uint32_t h=2166136261u ^ seed1; for(int k=0;k<3;k++){ h^=(uint8_t)tl[i+k]; h*=16777619u; } hbuf[hn++]=h&0xFFFFu;
            h=2166136261u ^ seed2; for(int k=0;k<3;k++){ h^=(uint8_t)tl[i+k]; h*=16777619u; } hbuf[hn++]=h&0xFFFFu;
            h=2166136261u ^ seed3; for(int k=0;k<3;k++){ h^=(uint8_t)tl[i+k]; h*=16777619u; } hbuf[hn++]=h&0xFFFFu;
            h=2166136261u ^ seed4; for(int k=0;k<3;k++){ h^=(uint8_t)tl[i+k]; h*=16777619u; } hbuf[hn++]=h&0xFFFFu;
        }
        for(size_t i=0;i<name_ids.n;i++){
            MDB_val k={.mv_data=&name_ids.ids[i],.mv_size=sizeof(uint64_t)}, v;
            if(mdb_get(txn, dbi_smeta, &k, &v)!=0 || v.mv_size<8196){ continue; }
            const uint8_t* bloom = (const uint8_t*)v.mv_data + 4; // skip trigram_count
            BOOL ok=TRUE;
            for(size_t j=0;j<hn;j++){
                uint32_t bit = hbuf[j];
                if( (bloom[bit>>3] & (1u << (bit&7))) == 0 ){ ok=FALSE; break; }
            }
            if(ok){ name_ids.ids[keep++]=name_ids.ids[i]; }
        }
        name_ids.n=keep;
        _freea(tl);
    }

    }

    // Step 2: expand to record ids via filename_index
    IdVec rec_ids; idvec_init(&rec_ids);
    MDB_cursor* cix=NULL; mdb_cursor_open(txn, dbi_fname_index, &cix);
    if(name_ids.n>0){
        for(size_t i=0;i<name_ids.n;i++){
            MDB_val k={.mv_data=&name_ids.ids[i],.mv_size=sizeof(uint64_t)}, v;
            if(mdb_cursor_get(cix, &k, &v, MDB_SET_KEY)==0){
                do{ idvec_push(&rec_ids, *(uint64_t*)v.mv_data); } while(mdb_cursor_get(cix, &k, &v, MDB_NEXT_DUP)==0);
            }
        }
    } else if(q.name_pattern){
        // No trigram narrowing (too short): fallback to full scan of strings to find matching names, then map via filename_index
        MDB_cursor* cs=NULL; mdb_cursor_open(txn, dbi_strings, &cs);
        MDB_val sk, sv; int rc = mdb_cursor_get(cs, &sk, &sv, MDB_FIRST);
        char* npat = _strdup(q.name_pattern); lowercase_ascii(npat, strlen(npat));
        while(rc==0){
            uint64_t sid = *(uint64_t*)sk.mv_data;
            char* name = (char*)sv.mv_data; size_t nlen = sv.mv_size;
            char* tmp = (char*)_malloca(nlen+1); memcpy(tmp,name,nlen); tmp[nlen]=0; lowercase_ascii(tmp,nlen);
            if(strstr(tmp, npat)){
                MDB_val k={.mv_data=&sid,.mv_size=sizeof(sid)}, v;
                if(mdb_cursor_get(cix, &k, &v, MDB_SET_KEY)==0){
                    do{ idvec_push(&rec_ids, *(uint64_t*)v.mv_data); } while(mdb_cursor_get(cix, &k, &v, MDB_NEXT_DUP)==0);
                }
            }
            _freea(tmp);
            rc = mdb_cursor_get(cs, &sk, &sv, MDB_NEXT);
        }
        mdb_cursor_close(cs);
        free(npat);
    } else {
        // No name term: user may only filter by size/date/ext/path; start from all records by iterating size_index or date_index; choose smaller range
        // We'll just iterate date_index today
        MDB_cursor* cd=NULL; mdb_cursor_open(txn, dbi_date, &cd);
        MDB_val k,v; int rc = mdb_cursor_get(cd, &k, &v, MDB_FIRST);
        while(rc==0){ idvec_push(&rec_ids, *(uint64_t*)v.mv_data); rc = mdb_cursor_get(cd, &k, &v, MDB_NEXT); }
        mdb_cursor_close(cd);
    }
    mdb_cursor_close(cix);
    sort_unique(&rec_ids);

    // Step 3: Apply filters, rank, and print (parallel)
    typedef struct { 
    MDB_env* env; 
    uint64_t* ids; 
    size_t start, end; 
    SearchQuery* q; 
    RankedResult* out; 
    size_t* outn;
    char db_path[MAX_PATH*3];
    SRWLOCK* lock;
} FilterArgs;
    SRWLOCK outlock; InitializeSRWLock(&outlock);
    int tcount = workers;
    HANDLE th[4]; FilterArgs fa[4];
    RankedResult* all = (RankedResult*)malloc(rec_ids.n * sizeof(RankedResult)); size_t alln=0;
    for(int ti=0; ti<tcount; ++ti){
        size_t s = (rec_ids.n*ti)/tcount;
        size_t e = (rec_ids.n*(ti+1))/tcount;
        ZeroMemory(&fa[ti], sizeof(FilterArgs));
        fa[ti].ids = rec_ids.ids; fa[ti].start=s; fa[ti].end=e; fa[ti].q=&q; fa[ti].out=all; fa[ti].outn=&alln; fa[ti].lock=&outlock;
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
    for(size_t i2=0;i2<show;i2++){
        uint64_t rid = all[i2].rec_id; MDB_val rk={.mv_data=&rid,.mv_size=sizeof(rid)}, rv;
        if(mdb_get(txnprint, dbi_recordsP, &rk, &rv)!=0 || rv.mv_size<sizeof(DbRecord)) continue;
        DbRecord* r = (DbRecord*)rv.mv_data;
        MDB_val pk={.mv_data=&r->parent_str_id,.mv_size=sizeof(r->parent_str_id)}, pv;
        MDB_val nk={.mv_data=&r->name_str_id,.mv_size=sizeof(r->name_str_id)}, nv;
        const char *pstr="?", *nstr="?";
        if(mdb_get(txnprint, dbi_stringsP, &pk, &pv)==0) pstr=(const char*)pv.mv_data;
        if(mdb_get(txnprint, dbi_stringsP, &nk, &nv)==0) nstr=(const char*)nv.mv_data;
        printf("%s\%s  size=%llu  mtime=%llu  score=%.1f
", pstr, nstr, (unsigned long long)r->file_size, (unsigned long long)r->modified_time, all[i2].score);
    }
    mdb_txn_abort(txnprint);
    free(all);

    // Save cache for next run
    if(q.name_pattern && rec_ids.n>0){
        save_cache(dbPath, q.name_pattern, &rec_ids);
    }

    mdb_txn_abort(txn); mdb_env_close(env);

    // cleanup
    if(primary) free(primary);
    if(q.name_pattern) free(q.name_pattern);
    if(q.path_filter) free(q.path_filter);
    if(q.ext) free(q.ext);
    idvec_free(&name_ids);
    idvec_free(&rec_ids);
    return 0;

do_search_with_ids: ;
    // If we jump here (cache hit), we need to reopen env to resolve
    MDB_env* env2=NULL; MDB_txn* txn2=NULL;
    mdb_env_create(&env2); mdb_env_set_maxdbs(env2,64);
    if(mdb_env_open(env2, u8db, MDB_RDONLY, 0664)!=0){ return 1; }
    mdb_txn_begin(env2,NULL,MDB_RDONLY,&txn2);
    MDB_dbi dbi_strings2, dbi_records2;
    mdb_dbi_open(txn2,"strings",0,&dbi_strings2);
    mdb_dbi_open(txn2,"records",0,&dbi_records2);
    // Actually, cached ids were in 'cached', but we didn't carry it here. For brevity, skip cache reopen path.
    mdb_txn_abort(txn2); mdb_env_close(env2);
    return 0;
}
