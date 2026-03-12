#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <direct.h>
#else
#include <unistd.h>
#endif

#include <lmdb.h>
#include "anything/anything.h"
#include "anything/database.h"

extern void extract_trigrams(const char* text, uint32_t** out_tris, size_t* out_count);
extern uint32_t build_bloom_for_name(const char* name_u8, uint8_t* bloom);
extern void build_bloom_hashes_simd(const char* tri, uint32_t* out4);
void bloom_set(uint8_t* bloom, uint32_t h){
    uint32_t bit = h & 0xFFFFu;
    bloom[bit>>3] |= (uint8_t)(1u << (bit & 7));
}
static BOOL bloom_has_local(const uint8_t* bloom, uint32_t h){
    uint32_t bit = h & 0xFFFFu;
    return (bloom[bit>>3] & (uint8_t)(1u << (bit & 7))) != 0;
}

static void make_temp_dir(wchar_t* out, size_t outsz){
#ifdef _WIN32
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    wchar_t dir[MAX_PATH];
    GetTempFileNameW(tmp, L"adb", 0, dir);
    /* GetTempFileNameW creates a file; delete it so we can make a dir */
    _wremove(dir);
    _wmkdir(dir);
    wcsncpy(out, dir, outsz);
    out[outsz-1] = 0;
#else
    char tmpl[] = "/tmp/adbXXXXXX";
    char* dir = mkdtemp(tmpl);
    assert(dir);
    mbstowcs(out, dir, outsz);
#endif
}

static int read_string_meta(const wchar_t* path, uint64_t id, StringMeta* out){
    MDB_env* env = NULL;
    if(mdb_env_create(&env) != 0) return 0;
    mdb_env_set_maxdbs(env, 32);
    char utf8[MAX_PATH * 3];
    size_t conv = wcstombs(utf8, path, sizeof(utf8));
    if(conv == (size_t)-1 || conv >= sizeof(utf8)){
        mdb_env_close(env);
        return 0;
    }
    utf8[conv] = '\0';
    if(mdb_env_open(env, utf8, MDB_RDONLY, 0664) != 0){ mdb_env_close(env); return 0; }
    MDB_txn* txn = NULL;
    if(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn) != 0){ mdb_env_close(env); return 0; }
    MDB_dbi dbi;
    if(mdb_dbi_open(txn, "strings", 0, &dbi) != 0){ mdb_txn_abort(txn); mdb_env_close(env); return 0; }
    MDB_val key = {.mv_data = (void*)&id, .mv_size = sizeof(id)};
    MDB_val val;
    int rc = mdb_get(txn, dbi, &key, &val);
    int ok = 0;
    if(rc == 0){
        MDB_val text;
        StringMeta meta;
        if(db_string_value_parse(&val, &text, &meta, NULL)){
            if(out) *out = meta;
            ok = 1;
        }
    }
    mdb_txn_abort(txn);
    mdb_env_close(env);
    return ok;
}

static void test_db_intern_wstring(){
    wchar_t path[MAX_PATH];
    make_temp_dir(path, MAX_PATH);
    Db* db = NULL;
    assert(db_create(path, 1, 1, &db));
    uint64_t id1 = db_intern_wstring(db, L"hello");
    assert(db_commit_write(db));
    uint64_t id2 = db_intern_wstring(db, L"hello");
    assert(id1 == id2);
    assert(db_commit_write(db));
    uint64_t id3 = db_intern_wstring(db, L"world");
    assert(id3 != id1);
    assert(db_commit_write(db));
    db_close(db);
}

static void test_trigram_bloom(){
    uint32_t* tris; size_t count;
    extract_trigrams("abcd", &tris, &count);
    assert(count == 2);
    assert(tris[0] == (((uint32_t)'a'<<16)|((uint32_t)'b'<<8)|'c'));
    assert(tris[1] == (((uint32_t)'b'<<16)|((uint32_t)'c'<<8)|'d'));
    static const char* long_name =
        "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz";
    uint8_t bloom[8192];
    uint32_t tc = build_bloom_for_name(long_name, bloom);
    assert(tc > 0);
    uint32_t hs[4];
    build_bloom_hashes_simd("abcd", hs);
    for(int i=0;i<4;i++){
        assert(bloom_has_local(bloom, hs[i]));
    }
    uint32_t short_tc = build_bloom_for_name("abc", bloom);
    assert(short_tc == 0);
}

static void test_bloom_cap(){
    size_t biglen = DB_BLOOM_MAX_BYTES + 1024;
    char* big = (char*)malloc(biglen + 1);
    assert(big);
    for(size_t i=0;i<biglen;i++) big[i] = 'a' + (i % 26);
    big[biglen] = '\0';
    uint8_t bloom[8192];
    uint32_t tc = build_bloom_for_name(big, bloom);
    free(big);
    size_t expected = DB_BLOOM_STRIDE_AFTER > 2 ? (DB_BLOOM_STRIDE_AFTER - 2) : 0;
    size_t rem = DB_BLOOM_MAX_BYTES > DB_BLOOM_STRIDE_AFTER ? (DB_BLOOM_MAX_BYTES - DB_BLOOM_STRIDE_AFTER) : 0;
    if(rem > 3){
        expected += ((rem - 3) / DB_BLOOM_STRIDE) + 1;
    }
    assert(tc == expected);
}

static void test_bloom_meta_sizes(){
    wchar_t path[MAX_PATH];
    make_temp_dir(path, MAX_PATH);
    Db* db = NULL;
    assert(db_create(path, 1, 1, &db));

    uint64_t parent = db_intern_wstring(db, L"parent");
    assert(parent != 0);
    assert(db_commit_write(db));

    struct {
        const char* text;
        uint8_t expected_hash;
        uint8_t expected_log2;
        BOOL    expect_bloom;
    } cases[] = {
        {"abcd", 0, 0, FALSE},
        {"this_is_twenty_chars!", 2, 11, TRUE},
        {"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefgh", 3, 12, TRUE},
        {"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz", 4, 13, TRUE}
    };

    for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i){
        const char* text = cases[i].text;
        size_t len = strlen(text);
        wchar_t wbuf[512];
        size_t converted = mbstowcs(wbuf, text, sizeof(wbuf)/sizeof(wbuf[0]));
        assert(converted != (size_t)-1);
        if(converted >= sizeof(wbuf)/sizeof(wbuf[0])) converted = (sizeof(wbuf)/sizeof(wbuf[0])) - 1;
        wbuf[converted] = L'\0';
        uint64_t name_id = db_intern_wstring(db, wbuf);
        assert(name_id != 0);
        assert(db_commit_write(db));

        char norm_buf[512];
        normalize_filename_utf8(text, norm_buf, sizeof(norm_buf));
        wchar_t wnorm[512];
        size_t norm_converted = mbstowcs(wnorm, norm_buf, sizeof(wnorm)/sizeof(wnorm[0]));
        assert(norm_converted != (size_t)-1);
        if(norm_converted >= sizeof(wnorm)/sizeof(wnorm[0])) norm_converted = (sizeof(wnorm)/sizeof(wnorm[0])) - 1;
        wnorm[norm_converted] = L'\0';
        uint64_t norm_id = db_intern_wstring(db, wnorm);
        assert(norm_id != 0);
        assert(db_commit_write(db));

        DbRecord rec = {0};
        rec.parent_str_id = parent;
        rec.name_str_id = name_id;
        rec.normalized_name_str_id = norm_id;
        rec.type = DB_REC_FILE;
        rec.file_size = 1;
        rec.creation_time = rec.modified_time = rec.access_time = 1;
        assert(db_put_records(db, &rec, 1));
        assert(db_commit_write(db));

        StringMeta meta;
        assert(read_string_meta(path, name_id, &meta));
        assert(meta.trigram_count >= (uint32_t)((len >= 3) ? (len - 2) : 0));
        assert(meta.hash_count == cases[i].expected_hash);
        if(cases[i].expect_bloom){
            assert(meta.bloom_log2 == cases[i].expected_log2);
            assert(meta.bloom_length > 0);
        } else {
            assert(meta.bloom_log2 == 0);
            assert(meta.bloom_length == 0);
        }
    }

    db_close(db);
}

static void test_db_put_records_and_get_by_path(){
    wchar_t path[MAX_PATH];
    make_temp_dir(path, MAX_PATH);
    Db* db = NULL;
    assert(db_create(path, 1, 1, &db));

    uint64_t parent_id = db_intern_wstring(db, L"parent");
    assert(db_commit_write(db));
    uint64_t name_id = db_intern_wstring(db, L"file.txt");
    assert(db_commit_write(db));

    char norm_buf[32];
    normalize_filename_utf8("file.txt", norm_buf, sizeof(norm_buf));
    wchar_t wnorm[32];
    size_t norm_len = mbstowcs(wnorm, norm_buf, sizeof(wnorm)/sizeof(wnorm[0]));
    assert(norm_len != (size_t)-1);
    if(norm_len >= sizeof(wnorm)/sizeof(wnorm[0])) norm_len = (sizeof(wnorm)/sizeof(wnorm[0])) - 1;
    wnorm[norm_len] = L'\0';
    uint64_t norm_id = db_intern_wstring(db, wnorm);
    assert(db_commit_write(db));

    DbRecord rec = {0};
    rec.parent_str_id = parent_id;
    rec.name_str_id = name_id;
    rec.normalized_name_str_id = norm_id;
    rec.type = DB_REC_FILE;
    rec.file_size = 1;
    rec.creation_time = rec.modified_time = rec.access_time = 1;
    assert(db_put_records(db, &rec, 1));
    assert(db_commit_write(db));
    const DbHeader* hdr = db_header(db);
    assert(hdr->record_count == 1);

    DbRecord out;
    assert(!db_get_record_by_path(db, L"parent", L"missing", &out));
    db_close(db);
}

int main(void){
    test_db_intern_wstring();
    test_trigram_bloom();
    test_bloom_cap();
    test_bloom_meta_sizes();
    test_db_put_records_and_get_by_path();
    printf("All database tests passed\n");
    return 0;
}
