#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>

#include "../database.h"

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
    char tmpl[] = "/tmp/adbXXXXXX";
    char* dir = mkdtemp(tmpl);
    assert(dir);
    mbstowcs(out, dir, outsz);
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
    uint8_t bloom[8192];
    uint32_t tc = build_bloom_for_name("abc", bloom);
    assert(tc == 1);
    uint32_t hs[4];
    build_bloom_hashes_simd("abc", hs);
    for(int i=0;i<4;i++){
        assert(bloom_has_local(bloom, hs[i]));
    }
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

static void test_db_put_records_and_get_by_path(){
    wchar_t path[MAX_PATH];
    make_temp_dir(path, MAX_PATH);
    Db* db = NULL;
    assert(db_create(path, 1, 1, &db));

    uint64_t parent_id = db_intern_wstring(db, L"parent");
    assert(db_commit_write(db));
    uint64_t name_id = db_intern_wstring(db, L"file.txt");
    assert(db_commit_write(db));

    DbRecord rec = {0};
    rec.parent_str_id = parent_id;
    rec.name_str_id = name_id;
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
    test_db_put_records_and_get_by_path();
    printf("All database tests passed\n");
    return 0;
}
