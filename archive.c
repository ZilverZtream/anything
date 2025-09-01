// archive.c - index files inside archive containers using libarchive
#include "archive.h"
#include "util.h"
#include <archive.h>
#include <archive_entry.h>

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
        if(name){
            wchar_t wname[MAX_LONG_PATH];
            to_wide(name, wname, MAX_LONG_PATH);
            DbRecord rec = {0};
            rec.type = DB_REC_FILE;
            rec.parent_str_id = parent_id;
            rec.name_str_id = db_intern_wstring(db, wname);
            db_put_records(db, &rec, 1);
        }
        archive_read_data_skip(a);
    }
    archive_read_close(a);
    archive_read_free(a);
    return TRUE;
}
