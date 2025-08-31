// archive.c - index files inside archive containers using libzip
#include "archive.h"
#include "util.h"
#include <zip.h>

BOOL index_archive(Db* db, const wchar_t* archive_path){
    if(!db || !archive_path) return FALSE;
    char u8[MAX_LONG_PATH*3];
    to_utf8(archive_path, u8, sizeof(u8));
    int err = 0;
    zip_t* z = zip_open(u8, 0, &err);
    if(!z) return FALSE;
    uint64_t parent_id = db_intern_wstring(db, archive_path);
    zip_int64_t count = zip_get_num_entries(z, 0);
    for(zip_int64_t i=0;i<count;i++){
        const char* name = zip_get_name(z, i, 0);
        if(!name) continue;
        wchar_t wname[MAX_LONG_PATH];
        to_wide(name, wname, MAX_LONG_PATH);
        DbRecord rec = {0};
        rec.type = DB_REC_FILE;
        rec.parent_str_id = parent_id;
        rec.name_str_id = db_intern_wstring(db, wname);
        db_put_records(db, &rec, 1);
    }
    zip_close(z);
    return TRUE;
}
