
// anything.c — Orchestrator
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <wchar.h>
#include <intrin.h>
#include <objbase.h>
#include <filter.h>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "query.lib")

#include "anything.h"
#include "database.h"
#include "util.h"
#include "lmdb.h"
#include "archive.h"
#include "plugin.h"
#include "scanner.h"

#include <stdbool.h>

static MPMCQueue g_live_updates;
static BOOL g_live_inited = FALSE;

void live_updates_init(void){
    if(!g_live_inited){
        MPMC_Init(&g_live_updates, 1<<12);
        g_live_inited = TRUE;
    }
}

BOOL live_updates_poll(LiveUpdate* out){
    if(!g_live_inited) return FALSE;
    void* p = NULL;
    if(!MPMC_Pop(&g_live_updates, &p)) return FALSE;
    if(!p) return FALSE;
    LiveUpdate* lu = (LiveUpdate*)p;
    *out = *lu;
    _aligned_free(lu);
    return TRUE;
}

static void push_live_update(const DbWorkItem* wi){
    if(!g_live_inited) return;
    LiveUpdate* lu = (LiveUpdate*)_aligned_malloc(sizeof(LiveUpdate), CACHE_LINE_SIZE);
    if(!lu) return;
    wcscpy_s(lu->parent_path, MAX_LONG_PATH, wi->parent_path);
    wcscpy_s(lu->name, MAX_PATH, wi->name);
    lu->op = wi->op;
    while(!MPMC_Push(&g_live_updates, lu)) Sleep(0);
}

#ifdef HAS_LIBEXIF
#include <libexif/exif-data.h>
#endif
#ifdef HAS_ID3TAG
#include <id3tag.h>
#endif

// ---- MPMC queue implementation ----
typedef enum { CONTENT_NONE, CONTENT_TEXT, CONTENT_IFILTER } ContentMode;

static ContentMode get_content_mode(const wchar_t* name){
    const wchar_t* ext = wcsrchr(name, L'.');
    if(!ext) return CONTENT_NONE;
    ext++;
    if(_wcsicmp(ext,L"txt")==0 || _wcsicmp(ext,L"md")==0 ||
       _wcsicmp(ext,L"c")==0   || _wcsicmp(ext,L"h")==0   ||
       _wcsicmp(ext,L"cpp")==0 || _wcsicmp(ext,L"hpp")==0 ||
       _wcsicmp(ext,L"py")==0  || _wcsicmp(ext,L"js")==0  ||
       _wcsicmp(ext,L"cs")==0  || _wcsicmp(ext,L"vb")==0  ||
       _wcsicmp(ext,L"r")==0   || _wcsicmp(ext,L"java")==0)
        return CONTENT_TEXT;

    if(_wcsicmp(ext,L"pdf")==0  || _wcsicmp(ext,L"doc")==0  ||
       _wcsicmp(ext,L"docx")==0 || _wcsicmp(ext,L"ppt")==0  ||
       _wcsicmp(ext,L"pptx")==0 || _wcsicmp(ext,L"xls")==0  ||
       _wcsicmp(ext,L"xlsx")==0)
        return CONTENT_IFILTER;

    return CONTENT_NONE;
}

static BOOL is_archive_file(const wchar_t* name){
    const wchar_t* ext = wcsrchr(name, L'.');
    if(!ext) return FALSE;
    ext++;
    return _wcsicmp(ext, L"zip")==0 || _wcsicmp(ext,L"rar")==0 || _wcsicmp(ext,L"7z")==0;
}

#define MAX_INDEXED_CONTENT (1024*1024) // 1MB

static wchar_t* extract_with_ifilter(const wchar_t* path){
    if(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)!=S_OK) return NULL;
    IFilter* filter=NULL;
    HRESULT hr = LoadIFilter(path, NULL, NULL, 0, 0, 0, &filter);
    if(FAILED(hr)) { CoUninitialize(); return NULL; }
    hr = filter->Init(IFILTER_INIT_APPLY_INDEX_ATTRIBUTES, 0, NULL, NULL);
    if(FAILED(hr)) { filter->Release(); CoUninitialize(); return NULL; }
    size_t cap=4096, len=0;
    wchar_t* out = (wchar_t*)malloc(cap*sizeof(wchar_t));
    if(!out){ filter->Release(); CoUninitialize(); return NULL; }
    for(;;){
        WCHAR buf[1024]; ULONG cch=1024;
        hr = filter->GetText(&cch, buf);
        if(hr!=S_OK || cch==0) break;
        if(len + cch + 1 > cap){
            cap = (cap + cch + 1)*2;
            wchar_t* tmp = (wchar_t*)realloc(out, cap*sizeof(wchar_t));
            if(!tmp){ free(out); out=NULL; break; }
            out = tmp;
        }
        memcpy(out+len, buf, cch*sizeof(wchar_t));
        len += cch;
    }
    if(out){
        out[len]=0;
    }
    filter->Release();
    CoUninitialize();
    return out;
}

static uint64_t index_file_content(Db* db, const wchar_t* parent, const wchar_t* name, uint64_t* author_out, uint64_t* title_out){
    *author_out = 0;
    *title_out = 0;
    ContentMode mode = get_content_mode(name);
    if(mode==CONTENT_NONE) return 0;
    wchar_t path[MAX_LONG_PATH];
    _snwprintf(path, MAX_LONG_PATH, L"%s\\%s", parent, name);

    wchar_t* wbuf = NULL;
    if(mode == CONTENT_TEXT){
        FILE* f = _wfopen(path, L"rb");
        if(!f) return 0;
        if(fseek(f,0,SEEK_END)!=0){ fclose(f); return 0; }
        long size = ftell(f);
        if(size < 0){ fclose(f); return 0; }
        if(size > MAX_INDEXED_CONTENT) size = MAX_INDEXED_CONTENT;
        rewind(f);
        char* buf = (char*)malloc((size_t)size + 1);
        if(!buf){ fclose(f); return 0; }
        size_t n = fread(buf,1,(size_t)size,f);
        fclose(f);
        buf[n]=0;
        for(size_t i=0;i<n;i++){ if(buf[i]==0){ buf[0]=0; break; } }
        if(buf[0]==0){ free(buf); return 0; }
        char* a = StrStrIA(buf, "author:");
        if(a){
            a += 7;
            while(*a==' '||*a=='\t') a++;
            char tmp[256]; size_t len=0;
            while(a[len] && a[len]!='\r' && a[len]!='\n' && len<255) len++;
            memcpy(tmp,a,len); tmp[len]=0;
            wchar_t wa[256]; to_wide(tmp, wa, 256);
            *author_out = db_intern_wstring(db, wa);
        }
        char* t = StrStrIA(buf, "title:");
        if(t){
            t += 6;
            while(*t==' '||*t=='\t') t++;
            char tmp[256]; size_t len=0;
            while(t[len] && t[len]!='\r' && t[len]!='\n' && len<255) len++;
            memcpy(tmp,t,len); tmp[len]=0;
            wchar_t wt[256]; to_wide(tmp, wt, 256);
            *title_out = db_intern_wstring(db, wt);
        }
        int wlen = MultiByteToWideChar(CP_UTF8,0,buf,-1,NULL,0);
        wbuf = (wchar_t*)malloc(sizeof(wchar_t)*wlen);
        if(wbuf) MultiByteToWideChar(CP_UTF8,0,buf,-1,wbuf,wlen);
        free(buf);
    } else if(mode == CONTENT_IFILTER){
        wbuf = extract_with_ifilter(path);
    }
    if(!wbuf) return 0;
    wchar_t* meta_a = StrStrIW(wbuf, L"author:");
    if(meta_a){
        meta_a += 7;
        while(*meta_a==L' '||*meta_a==L'\t') meta_a++;
        wchar_t tmp[256]; size_t len=0;
        while(meta_a[len] && meta_a[len]!=L'\r' && meta_a[len]!=L'\n' && len<255) len++;
        wcsncpy_s(tmp,256,meta_a,len);
        *author_out = db_intern_wstring(db, tmp);
    }
    wchar_t* meta_t = StrStrIW(wbuf, L"title:");
    if(meta_t){
        meta_t += 6;
        while(*meta_t==L' '||*meta_t==L'\t') meta_t++;
        wchar_t tmp[256]; size_t len=0;
        while(meta_t[len] && meta_t[len]!=L'\r' && meta_t[len]!=L'\n' && len<255) len++;
        wcsncpy_s(tmp,256,meta_t,len);
        *title_out = db_intern_wstring(db, tmp);
    }
    uint64_t id = db_intern_wstring(db, wbuf);
    free(wbuf);
    return id;
}

static void extract_exif_metadata(Db* db, const wchar_t* path, DbRecord* r){
#ifdef HAS_LIBEXIF
    char u8[MAX_LONG_PATH];
    to_utf8(path, u8, sizeof(u8));
    ExifData* ed = exif_data_new_from_file(u8);
    if(ed){
        ExifEntry* e = exif_content_get_entry(ed->ifd[EXIF_IFD_0], EXIF_TAG_MODEL);
        if(e){
            char buf[256]={0}; exif_entry_get_value(e, buf, sizeof(buf));
            wchar_t wbuf[256]; to_wide(buf, wbuf, 256);
            r->camera_str_id = db_intern_wstring(db, wbuf);
        }
        e = exif_content_get_entry(ed->ifd[EXIF_IFD_EXIF], EXIF_TAG_LENS_MODEL);
        if(e){
            char buf[256]={0}; exif_entry_get_value(e, buf, sizeof(buf));
            wchar_t wbuf[256]; to_wide(buf, wbuf, 256);
            r->lens_str_id = db_intern_wstring(db, wbuf);
        }
        exif_data_unref(ed);
    }
#else
    (void)db; (void)path; (void)r;
#endif
}

static void extract_id3_metadata(Db* db, const wchar_t* path, DbRecord* r){
#ifdef HAS_ID3TAG
    char u8[MAX_LONG_PATH];
    to_utf8(path, u8, sizeof(u8));
    struct id3_file* f = id3_file_open(u8, ID3_FILE_MODE_READONLY);
    if(f){
        struct id3_tag* tag = id3_file_tag(f);
        struct id3_frame* fr;
        if((fr = id3_tag_findframe(tag, "TPE1",0))){
            const id3_ucs4_t* uc = id3_field_getstrings(&fr->fields[1],0);
            if(uc){
                char buf[256]; id3_ucs4_utf8duplicate(uc,(id3_utf8_t*)buf,sizeof(buf));
                wchar_t wbuf[256]; to_wide(buf, wbuf, 256);
                r->artist_str_id = db_intern_wstring(db, wbuf);
            }
        }
        if((fr = id3_tag_findframe(tag, "TALB",0))){
            const id3_ucs4_t* uc = id3_field_getstrings(&fr->fields[1],0);
            if(uc){
                char buf[256]; id3_ucs4_utf8duplicate(uc,(id3_utf8_t*)buf,sizeof(buf));
                wchar_t wbuf[256]; to_wide(buf, wbuf, 256);
                r->album_str_id = db_intern_wstring(db, wbuf);
            }
        }
        id3_file_close(f);
    }
#else
    (void)db; (void)path; (void)r;
#endif
}
BOOL MPMC_Init(MPMCQueue* q, LONG pow2_size){
    if(!q) return FALSE;
    LONG size=1; while(size<pow2_size) size<<=1;
    q->mask = size-1;
    size_t cells_size = sizeof(MPMCCell) * (size_t)size;
    q->cells = (MPMCCell*)_aligned_malloc(cells_size, CACHE_LINE_SIZE);
    if(!q->cells) return FALSE;
    ZeroMemory(q->cells, cells_size);
    for(LONG i=0;i<size;i++){ q->cells[i].seq = i; }
    q->head=0; q->tail=0;
    return TRUE;
}
void MPMC_Destroy(MPMCQueue* q){
    if(!q || !q->cells) return;
    _aligned_free(q->cells); q->cells=NULL;
}
BOOL MPMC_Push(MPMCQueue* q, void* data){
    MPMCCell* cell; LONG64 pos = q->head;
    for(;;){
        cell = &q->cells[pos & q->mask];
        LONG64 seq = cell->seq;
        if(seq == pos){
            if(InterlockedCompareExchange64(&q->head, pos+1, pos)==pos){
                cell->data = data;
                _ReadWriteBarrier();
                cell->seq = pos+1;
                return TRUE;
            }
        } else if(seq < pos) {
            return FALSE; // full
        } else {
            pos = q->head;
        }
        SwitchToThread();
    }
}
BOOL MPMC_Pop(MPMCQueue* q, void** out){
    MPMCCell* cell; LONG64 pos = q->tail;
    for(;;){
        cell = &q->cells[pos & q->mask];
        LONG64 seq = cell->seq;
        if(seq == pos+1){
            if(InterlockedCompareExchange64(&q->tail, pos+1, pos)==pos){
                void* d = cell->data;
                _ReadWriteBarrier();
                cell->seq = pos + q->mask + 1;
                *out = d;
                return TRUE;
            }
        } else if(seq < pos+1){
            return FALSE; // empty
        } else {
            pos = q->tail;
        }
        SwitchToThread();
    }
}

// ---- Writer context & thread ----
typedef struct WriterCtx {
    Db* db;
    int batch_size;
    volatile BOOL done;
    MPMCQueue queue;
    HANDLE cancel_event;
    size_t grow_attempts;
} WriterCtx;

static BOOL put_batch_with_growth(WriterCtx* ctx, DbRecord* buf, size_t in_batch){
    for(;;){
        if(db_put_records(ctx->db, buf, in_batch)){
            return TRUE;
        }
        int err = db_last_error(ctx->db);
        if(err == MDB_MAP_FULL){
            db_abort_write(ctx->db);
            size_t cur = db_current_mapsize(ctx->db);
            size_t newsize = cur * 2;
            if(newsize > db_max_mapsize(ctx->db)){
                fprintf(stderr, "DB mapsize at max; cannot grow beyond %llu bytes\n", (unsigned long long)db_max_mapsize(ctx->db));
                return FALSE;
            }
            if(!db_set_mapsize(ctx->db, newsize)){
                fprintf(stderr, "db_set_mapsize failed (err=%d)\n", db_last_error(ctx->db));
                return FALSE;
            }
            if(!db_begin_write(ctx->db)) return FALSE;
            ctx->grow_attempts++;
            continue; // retry put
        } else {
            fprintf(stderr, "db_put_records failed (err=%d)\n", err);
            return FALSE;
        }
    }
}

static DWORD WINAPI DbWriterThread(void* p){
    WriterCtx* ctx = (WriterCtx*)p;
    if(!db_begin_write(ctx->db)) return 1;
    size_t in_batch = 0;
    DbRecord* buf = (DbRecord*)malloc(sizeof(DbRecord) * ctx->batch_size);
    if(!buf) return 1;
    ZeroMemory(buf, sizeof(DbRecord)*ctx->batch_size);

    for(;;){
        void* item = NULL;
        if(!MPMC_Pop(&ctx->queue, &item)){
            if(ctx->done) break;
            Sleep(1);
            continue;
        }
        if(item == NULL) break; // sentinel
        DbWorkItem* wi = (DbWorkItem*)item;
        push_live_update(wi);
        if(wi->op == WI_DELETE){
            db_delete_path(ctx->db, wi->parent_path, wi->name);
            _aligned_free(wi);
            continue;
        }
        DbRecord r = {0};
        r.type = (wi->attributes & FILE_ATTRIBUTE_DIRECTORY) ? DB_REC_DIR : DB_REC_FILE;
        r.parent_str_id = db_intern_wstring(ctx->db, wi->parent_path);
        r.name_str_id   = db_intern_wstring(ctx->db, wi->name);
        r.file_size     = wi->file_size;
        r.creation_time = wi->creation_time;
        r.modified_time = wi->modified_time;
        r.access_time   = wi->access_time;
        r.attributes    = wi->attributes;
        if(r.type == DB_REC_FILE){
            r.content_str_id = index_file_content(ctx->db, wi->parent_path, wi->name, &r.author_str_id, &r.title_str_id);
            wchar_t fpath[MAX_LONG_PATH];
            _snwprintf(fpath, MAX_LONG_PATH, L"%s\\%s", wi->parent_path, wi->name);
            extract_exif_metadata(ctx->db, fpath, &r);
            extract_id3_metadata(ctx->db, fpath, &r);
            if(is_archive_file(wi->name)){
                index_archive(ctx->db, fpath);
            }
        }
        _aligned_free(wi);

        buf[in_batch++] = r;
        if(in_batch >= (size_t)ctx->batch_size){
            if(!put_batch_with_growth(ctx, buf, in_batch)) { free(buf); return 1; }
            if(!db_commit_write(ctx->db)) { free(buf); return 1; }
            if(!db_begin_write(ctx->db))  { free(buf); return 1; }
            in_batch = 0;
        }
    }
    if(in_batch){
        if(!put_batch_with_growth(ctx, buf, in_batch)) { free(buf); return 1; }
        if(!db_commit_write(ctx->db)) { free(buf); return 1; }
    } else {
        db_commit_write(ctx->db);
    }
    free(buf);
    return 0;
}

// ---- CLI parsing ----
typedef struct {
    wchar_t dbPath[MAX_PATH];
    wchar_t rootPath[MAX_LONG_PATH];
    int threads;
    int batch;
    BOOL use_ntfs;
    BOOL tail_changes;
    BOOL all_drives;
} Args;

static void usage(void){
    wprintf(L"anything.exe index --db <path> (--root <folder> | --all-drives) [--threads N] [--batch N] [--ntfs] [--tail]\n");
}

static BOOL parse_args(int argc, wchar_t** argv, Args* a){
    ZeroMemory(a, sizeof(*a));
    a->threads = 8; a->batch=50000; a->use_ntfs=FALSE; a->tail_changes=FALSE; a->all_drives=FALSE;
    for(int i=1;i<argc;i++){
        if(wcscmp(argv[i], L"index")==0){ continue; }
        else if(wcscmp(argv[i], L"--db")==0 && i+1<argc){ wcscpy_s(a->dbPath, MAX_PATH, argv[++i]); }
        else if(wcscmp(argv[i], L"--root")==0 && i+1<argc){ wcscpy_s(a->rootPath, MAX_LONG_PATH, argv[++i]); }
        else if(wcscmp(argv[i], L"--threads")==0 && i+1<argc){ a->threads = _wtoi(argv[++i]); }
        else if(wcscmp(argv[i], L"--batch")==0 && i+1<argc){ a->batch = _wtoi(argv[++i]); }
        else if(wcscmp(argv[i], L"--ntfs")==0){ a->use_ntfs = TRUE; }
        else if(wcscmp(argv[i], L"--tail")==0){ a->tail_changes = TRUE; }
        else if(wcscmp(argv[i], L"--all-drives")==0){ a->all_drives = TRUE; }
        else { usage(); return FALSE; }
    }
    if(!a->dbPath[0] || (!a->rootPath[0] && !a->all_drives)){ usage(); return FALSE; }
    if(a->threads<1) a->threads=1; if(a->threads>MAX_THREADS) a->threads=MAX_THREADS;
    if(a->batch<1000) a->batch=1000;
    return TRUE;
}

static DWORD WINAPI scan_drive_thread(void* p){
    struct { wchar_t root[8]; WriterCtx* ctx; BOOL use_ntfs; int threads; } *in = p;
    (void)in->use_ntfs; // selection now handled internally
    FileScanner* fs = FileScanner_Start(in->root, in->threads, &in->ctx->queue, in->ctx->cancel_event);
    if(fs){
        FileScanner_Wait(fs);
        FileScanner_Free(fs);
    }
    free(in);
    return 0;
}

int wmain(int argc, wchar_t** argv){
    Args args;
    live_updates_init();
    if(!parse_args(argc, argv, &args)) return 1;

    Db* db=NULL;
    if(!db_create(args.dbPath, /*init_mb*/1024, /*max_mb*/16384, &db)){
        fwprintf(stderr, L"Failed to create/open DB at %s (err=%d)\n", args.dbPath, db?db_last_error(db):0);
        return 1;
    }

    WriterCtx ctx = {0};
    ctx.db = db; ctx.batch_size = args.batch; ctx.done=FALSE; ctx.cancel_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if(!MPMC_Init(&ctx.queue, 1<<16)){
        fwprintf(stderr, L"MPMC_Init failed\n"); db_close(db); return 1;
    }
    PluginHost ph = { &ctx.queue, ctx.cancel_event };
    Plugin_LoadAll(L"plugins", &ph);
    HANDLE writer = CreateThread(NULL,0,DbWriterThread,&ctx,0,NULL);

    HANDLE drive_threads[26]; int drive_count=0;
    // Incremental index state
    IndexState st={0}; BOOL has_state = db_get_index_state(db, &st);
    // TODO: compute drive signatures; for now, always scan (production: compare st.drive_signatures)

    if(args.all_drives){
        DWORD mask = GetLogicalDrives();
        for(int i=0;i<26;i++){
            if(mask & (1u<<i)){
                wchar_t root[8]; swprintf(root, 8, L"%c:\\", L'A'+i);
                UINT type = GetDriveTypeW(root);
                if(type==DRIVE_FIXED || type==DRIVE_REMOVABLE){
                    struct { wchar_t root[8]; WriterCtx* ctx; BOOL use_ntfs; int threads; } *in = (struct { wchar_t root[8]; WriterCtx* ctx; BOOL use_ntfs; int threads; }*)malloc(sizeof(*in));
                    wcscpy_s(in->root, 8, root);
                    in->ctx=&ctx; in->use_ntfs=args.use_ntfs; in->threads=args.threads;
                    drive_threads[drive_count++] = CreateThread(NULL,0,scan_drive_thread,in,0,NULL);
                }
            }
        }
    } else {
        struct { wchar_t root[8]; WriterCtx* ctx; BOOL use_ntfs; int threads; } *in = (struct { wchar_t root[8]; WriterCtx* ctx; BOOL use_ntfs; int threads; }*)malloc(sizeof(*in));
        wcscpy_s(in->root, 8, args.rootPath);
        in->ctx=&ctx; in->use_ntfs=args.use_ntfs; in->threads=args.threads;
        drive_threads[drive_count++] = CreateThread(NULL,0,scan_drive_thread,in,0,NULL);
    }

    WaitForMultipleObjects(drive_count, drive_threads, TRUE, INFINITE);
    for(int i=0;i<drive_count;i++) CloseHandle(drive_threads[i]);

    Plugin_ScanAll();

    if(args.tail_changes && !args.all_drives){
        HANDLE tailer = StartUSNTailer(args.rootPath, &ctx.queue, ctx.cancel_event);
        if(tailer){
            wprintf(L"Tailing changes on %s. Press Ctrl+C to stop...\n", args.rootPath);
            Sleep(5000); // demo
        }
    }

    ctx.done = TRUE;
    MPMC_Push(&ctx.queue, NULL); // sentinel
    WaitForSingleObject(writer, INFINITE);
    CloseHandle(writer);

    const DbHeader* header = db_header(db);
    if(header){
        wprintf(L"Total records: %llu, strings: %llu, map: %llu MiB\n",
            (unsigned long long)header->record_count,
            (unsigned long long)header->string_count,
            (unsigned long long)(header->map_size_bytes/1024/1024));
    }
    Plugin_UnloadAll();
    db_close(db);
    MPMC_Destroy(&ctx.queue);
    CloseHandle(ctx.cancel_event);
    return 0;
}
