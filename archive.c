// archive.c - index files inside archive containers using libarchive
#include "archive.h"
#include "util.h"
#include <archive.h>
#include <archive_entry.h>
#include <string.h>
#include <ctype.h>

// Resolve and validate archive entry paths to avoid directory traversal.
// The path is URL-decoded, normalized and ensured to stay within the
// extraction root by rejecting any attempt to escape via ".." segments or
// absolute paths. On success, the canonical relative path is written to out.
static BOOL normalize_archive_path(const char* name, char* out, size_t outcch){
    if(!name || !out || outcch==0) return FALSE;
    if(name[0]=='/' || strchr(name,'\\') || strchr(name,':')) return FALSE;
    char decoded[MAX_LONG_PATH*3];
    size_t di=0;
    for(size_t i=0; name[i] && di<sizeof(decoded)-1; ++i){
        if(name[i]=='%'){
            int hi = isxdigit((unsigned char)name[i+1]) ? (isdigit((unsigned char)name[i+1]) ? name[i+1]-'0' : tolower((unsigned char)name[i+1])-'a'+10) : -1;
            int lo = isxdigit((unsigned char)name[i+2]) ? (isdigit((unsigned char)name[i+2]) ? name[i+2]-'0' : tolower((unsigned char)name[i+2])-'a'+10) : -1;
            if(hi<0 || lo<0) return FALSE;
            decoded[di++] = (char)((hi<<4)|lo);
            i+=2;
        }else{
            decoded[di++] = name[i];
        }
    }
    decoded[di]=0;
    // Split into components and resolve '.' and '..'
    char* segs[MAX_LONG_PATH];
    size_t seg_count=0;
    char* p = decoded;
    while(p && *p){
        char* next = strchr(p,'/');
        if(next) *next = 0;
        if(strcmp(p,"..") == 0){
            if(seg_count==0) return FALSE; // escape attempt
            seg_count--; // pop previous component
        }else if(strcmp(p,".") != 0 && *p){
            if(seg_count >= MAX_LONG_PATH) return FALSE;
            segs[seg_count++] = p;
        }
        if(!next) break;
        p = next+1;
    }
    size_t pos=0;
    for(size_t i=0; i<seg_count; ++i){
        size_t len = strlen(segs[i]);
        if(pos && pos<outcch-1) out[pos++] = '/';
        if(pos + len >= outcch) return FALSE;
        memcpy(out+pos, segs[i], len);
        pos += len;
    }
    out[pos] = 0;
    return TRUE;
}

BOOL index_archive(Db* db, const wchar_t* archive_path){
    if(!db || !archive_path) return FALSE;
    char u8[MAX_LONG_PATH*3];
    to_utf8(archive_path, u8, sizeof(u8));
    struct archive* a = archive_read_new();
    if(!a) return FALSE;
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    if(archive_read_open_filename(a, u8, 10240) != ARCHIVE_OK){
        archive_read_free(a);
        return FALSE;
    }
    uint64_t parent_id = db_intern_wstring(db, archive_path);
    struct archive_entry* entry;
    while(archive_read_next_header(a, &entry) == ARCHIVE_OK){
        const char* name = archive_entry_pathname(entry);
        char canon[MAX_LONG_PATH*3];
        if(!normalize_archive_path(name, canon, sizeof(canon))){
            archive_read_data_skip(a);
            continue;
        }
        wchar_t wname[MAX_LONG_PATH];
        to_wide(canon, wname, MAX_LONG_PATH);
        DbRecord rec = {0};
        rec.type = DB_REC_FILE;
        rec.parent_str_id = parent_id;
        rec.name_str_id = db_intern_wstring(db, wname);
        db_put_records(db, &rec, 1);
        archive_read_data_skip(a);
    }
    archive_read_close(a);
    archive_read_free(a);
    return TRUE;
}
