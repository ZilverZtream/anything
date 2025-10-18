#include "anything/util.h"
#include <assert.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

static void test_normalize_filename_utf8(){
    char out[256];
    normalize_filename_utf8("Example.File.txt", out, sizeof(out));
    assert(strcmp(out, "example file") == 0);
    normalize_filename_utf8("multi.part.name.ext", out, sizeof(out));
    assert(strcmp(out, "multi part name") == 0);
    char inplace[256] = "Test-File.NAME";
    normalize_filename_utf8(inplace, inplace, sizeof(inplace));
    assert(strcmp(inplace, "test file") == 0);
}

static void test_hash64_crc64_sha1(){
    const char* msg = "hello";
    uint64_t h = hash64(msg, strlen(msg));
    assert(h == 0x005a0d15131ec7a1ULL);
    uint64_t c = crc64(msg, strlen(msg));
    assert(c == 0x00000000df03cd79ULL);
    unsigned char sha[20];
    sha1(msg, strlen(msg), sha);
    unsigned char expect[20] = {
        0xaa,0xf4,0xc6,0x1d,0xdc,0xc5,0xe8,0xa2,0xda,0xbe,
        0xde,0x0f,0x3b,0x48,0x2c,0xd9,0xae,0xa9,0x43,0x4d
    };
    assert(memcmp(sha, expect, 20) == 0);
}

static void test_levenshtein_distance(){
    assert(levenshtein_distance("kitten",6,"sitting",7) == 3);
    assert(levenshtein_distance("",0,"abc",3) == 3);
    assert(levenshtein_distance("abc",3,"abc",3) == 0);
}

static void test_fuzzy_match(){
    assert(fuzzy_match("hello","hello",0));
    assert(fuzzy_match("hello","hullo",1));
    assert(!fuzzy_match("hello","world",2));
    assert(!fuzzy_match("short","longer",1));
    // regression: ensure matches are found even when no trigram is shared
    assert(fuzzy_match("abxd","abcd",1));
}

static void test_bm25_score(){
    float s1 = bm25_score(3,100,200.0f,1000,10);
    assert(fabsf(s1 - 8.6807229f) < 1e-5f);
    float s2 = bm25_score(1,10,10.0f,100,1);
    assert(fabsf(s2 - 4.2096554f) < 1e-5f);
}

static void test_path_join_dirname(){
    wchar_t out[MAX_PATH];
    // path_join with root path and automatic separator
    assert(path_join(out, MAX_PATH, L"C:\\", L"file.txt"));
    assert(wcscmp(out, L"C:\\file.txt") == 0);
    // path_join where first part lacks trailing slash
    assert(path_join(out, MAX_PATH, L"C:\\base", L"child"));
    assert(wcscmp(out, L"C:\\base\\child") == 0);
    // path_join with empty second component
    assert(path_join(out, MAX_PATH, L"C:\\base", L""));
    assert(wcscmp(out, L"C:\\base\\") == 0);
    // path_join with empty first component
    assert(path_join(out, MAX_PATH, L"", L"solo"));
    assert(wcscmp(out, L"solo") == 0);

    // path_dirname basic path
    assert(path_dirname(L"C:\\base\\child.txt", out, MAX_PATH));
    assert(wcscmp(out, L"C:\\base") == 0);
    // path_dirname with trailing slash
    assert(path_dirname(L"C:\\base\\sub\\", out, MAX_PATH));
    assert(wcscmp(out, L"C:\\base\\sub") == 0);
    // path_dirname of root
    assert(path_dirname(L"C:\\", out, MAX_PATH));
    assert(wcscmp(out, L"C:") == 0);
    // path_dirname without separator
    assert(!path_dirname(L"noslash", out, MAX_PATH));
    // path_dirname empty string
    assert(!path_dirname(L"", out, MAX_PATH));
}

int main(void){
    test_normalize_filename_utf8();
    test_hash64_crc64_sha1();
    test_levenshtein_distance();
    test_fuzzy_match();
    test_bm25_score();
    test_path_join_dirname();
    printf("All util tests passed\n");
    return 0;
}
