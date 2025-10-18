
#ifndef UTIL_H
#define UTIL_H
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <wchar.h>
#include "result.h"

#ifdef __cplusplus
extern "C" {
#endif

// configurable sort buffer size (default 256MB)
extern size_t g_sort_buffer_size;

// dynamic work memory helper
size_t dynamic_work_mem(void);

// aligned allocation helpers
void* aligned_malloc(size_t size, size_t alignment);
void aligned_free(void* p);

void lowercase_ascii(char* s, size_t n);
void lowercase_wchar(wchar_t* s);
void make_long_path(const wchar_t* in, wchar_t* out, size_t outcch);
BOOL path_join(wchar_t* dst, size_t dstcch, const wchar_t* a, const wchar_t* b);
BOOL path_dirname(const wchar_t* path, wchar_t* out, size_t outcch);
BOOL get_drive_root(const wchar_t* any_path, wchar_t* root, size_t cch);
uint64_t filetime_days(uint64_t ft);
BOOL get_file_info_basic(const wchar_t* full, uint32_t* attrs, uint64_t* size, uint64_t* ctime, uint64_t* mtime, uint64_t* atime);
void split_extension_utf8(const char* name_utf8, char* ext_out, size_t ext_len);
void to_utf8(const wchar_t* w, char* u8, size_t u8cap);
void to_wide(const char* u8, wchar_t* w, size_t wcap);
uint64_t hash64(const void* data, size_t len);
uint64_t crc64_update(uint64_t crc, const void* data, size_t len);
uint64_t crc64(const void* data, size_t len);

struct CancelToken;
typedef void (*crc64_progress_fn)(uint64_t processed_bytes, uint64_t total_bytes, void* user_data);
uint64_t crc64_file(const wchar_t* path,
                    const struct CancelToken* cancel_token,
                    crc64_progress_fn progress_cb,
                    void* progress_ctx,
                    BOOL* out_success);
void sha1(const void* data, size_t len, uint8_t out[20]);
void compute_drive_signature(const wchar_t* drive, uint8_t sig[32]);
float bm25_score(int tf, int doc_len, float avg_doc_len, int docs_total, int docs_with_term);

int levenshtein_distance(const char* a, size_t alen, const char* b, size_t blen);
BOOL fuzzy_match(const char* text, const char* pattern, int max_dist);
void normalize_filename_utf8(const char* name_utf8, char* out, size_t outcap);

// SIMD search
BOOL is_avx2_supported(void);
BOOL avx2_contains(const char* haystack, size_t hlen, const char* needle, size_t nlen);

// packed sort buffer helpers
typedef struct {
    uint8_t* data;
    size_t len;
    size_t cap;
} SortBuffer;

void sb_init(SortBuffer* sb);
void sb_free(SortBuffer* sb);
Result sb_pack_str(SortBuffer* sb, const char* s);
Result sb_pack_u64(SortBuffer* sb, uint64_t v);

// incremental hash helpers for sort keys
uint64_t hash64_add(uint64_t h, const void* data, size_t len);
uint64_t hash64_sort_key(const SortBuffer* sb);

// external sort spilling to disk when exceeding sort_buffer_size
BOOL external_sort(const wchar_t* tmpdir, void* base, size_t n, size_t size,
                   int (*cmp)(const void*, const void*));

#ifdef __cplusplus
}
#endif
#endif
