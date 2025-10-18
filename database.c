
// database.c — LMDB store with multiple indices and trigram index
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <wchar.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shlwapi.h>
#include <immintrin.h>
#include <limits.h>
#pragma comment(lib, "shlwapi.lib")

#include "database.h"
#include "anything.h"
#include "util.h"
#include "lmdb.h"

#ifndef _strdup
#define _strdup strdup
#endif
#ifndef ARRAYSIZE
#define ARRAYSIZE(a) (sizeof(a)/sizeof((a)[0]))
#endif

#if defined(_MSC_VER)
#define THREAD_LOCAL __declspec(thread)
#else
#define THREAD_LOCAL __thread
#endif

typedef struct {
    uint32_t trigram_count;
    uint8_t  hash_count;
    uint8_t  bloom_log2;
    uint8_t  magic0;
    uint8_t  magic1;
    uint64_t bloom_offset; // offset in bloom file
    uint32_t bloom_length;
} StringMeta;

#define STRING_META_MAGIC0 'B'
#define STRING_META_MAGIC1 'F'

typedef struct {
    volatile uint64_t hash;
    volatile LONG64   stamp;
    volatile uint32_t string_id;
    volatile char*    string;
} StringCache;

#define STRING_CACHE_SIZE 65536u
#define CACHE_PARTITIONS 64u
#define PARTITION_SIZE (STRING_CACHE_SIZE / CACHE_PARTITIONS)
#define PARTITION_MASK (PARTITION_SIZE-1u)

static StringCache g_string_cache[CACHE_PARTITIONS][PARTITION_SIZE];
static const char* const STRING_CACHE_BUSY = (const char*)(intptr_t)1;

static volatile LONG64 g_string_cache_clock = 0;

static inline char* atomic_load_char(volatile char** p){
    return (char*)InterlockedCompareExchangePointer((PVOID*)p, NULL, NULL);
}

static uint64_t string_cache_lookup(const char* s, uint64_t h){
    if(!s) return 0;
    size_t partition = (size_t)(h % CACHE_PARTITIONS);
    size_t mask = PARTITION_MASK;
    size_t idx = ((size_t)(h / CACHE_PARTITIONS)) & mask;
    size_t step = (((size_t)(h >> 32) & mask) | 1u);
    if(step == 0) step = 1u;
    StringCache* cache = g_string_cache[partition];
    for(size_t probe = 0; probe < PARTITION_SIZE; ++probe){
        StringCache* c = &cache[idx];
        char* str = atomic_load_char((volatile char**)&c->string);
        if(!str) return 0;
        if(str == STRING_CACHE_BUSY){
            idx = (idx + step) & mask;
            continue;
        }
        if(c->hash == h && strcmp(str, s) == 0){
            uint32_t cached_id = (uint32_t)InterlockedCompareExchange((volatile LONG*)&c->string_id, 0, 0);
            if(cached_id != 0){
                LONG64 stamp = InterlockedIncrement64(&g_string_cache_clock);
                InterlockedExchange64(&c->stamp, stamp);
                return (uint64_t)cached_id;
            }
        }
        idx = (idx + step) & mask;
    }
    return 0;
}

static void string_cache_insert(const char* s, uint64_t h, uint64_t id){
    if(!s || id == 0 || id > UINT32_MAX) return;
    size_t partition = (size_t)(h % CACHE_PARTITIONS);
    size_t mask = PARTITION_MASK;
    size_t idx = ((size_t)(h / CACHE_PARTITIONS)) & mask;
    size_t step = (((size_t)(h >> 32) & mask) | 1u);
    if(step == 0) step = 1u;
    size_t best_idx = idx;
    LONG64 best_stamp = LLONG_MAX;
    uint32_t id32 = (uint32_t)id;
    LONG64 stamp = InterlockedIncrement64(&g_string_cache_clock);
    StringCache* cache = g_string_cache[partition];
    for(size_t probe = 0; probe < PARTITION_SIZE; ++probe){
        StringCache* c = &cache[idx];
        char* str = atomic_load_char((volatile char**)&c->string);
        if(!str){
            if(InterlockedCompareExchangePointer((PVOID*)&c->string, (PVOID)STRING_CACHE_BUSY, NULL) == NULL){
                c->hash = h;
                InterlockedExchange((volatile LONG*)&c->string_id, (LONG)id32);
                InterlockedExchange64(&c->stamp, stamp);
                char* dup = _strdup(s);
                if(!dup){
                    InterlockedExchange((volatile LONG*)&c->string_id, 0);
                    InterlockedExchange64(&c->stamp, 0);
                    InterlockedExchangePointer((PVOID*)&c->string, NULL);
                    return;
                }
                MemoryBarrier();
                InterlockedExchangePointer((PVOID*)&c->string, dup);
                return;
            }
            continue;
        }
        if(str == STRING_CACHE_BUSY){
            idx = (idx + step) & mask;
            continue;
        }
        if(c->hash == h && strcmp(str, s) == 0){
            InterlockedExchange((volatile LONG*)&c->string_id, (LONG)id32);
            InterlockedExchange64(&c->stamp, stamp);
            return;
        }
        LONG64 entry_stamp = InterlockedCompareExchange64(&c->stamp, 0, 0);
        if(entry_stamp < best_stamp){
            best_stamp = entry_stamp;
            best_idx = idx;
        }
        idx = (idx + step) & mask;
    }
    StringCache* victim = &cache[best_idx];
    for(;;){
        char* expected = atomic_load_char((volatile char**)&victim->string);
        if(expected == STRING_CACHE_BUSY) continue;
        if(InterlockedCompareExchangePointer((PVOID*)&victim->string, (PVOID)STRING_CACHE_BUSY, expected) == expected){
            if(expected) free(expected);
            victim->hash = h;
            InterlockedExchange((volatile LONG*)&victim->string_id, (LONG)id32);
            InterlockedExchange64(&victim->stamp, stamp);
            char* dup = _strdup(s);
            if(!dup){
                InterlockedExchange((volatile LONG*)&victim->string_id, 0);
                InterlockedExchange64(&victim->stamp, 0);
                InterlockedExchangePointer((PVOID*)&victim->string, NULL);
                return;
            }
            MemoryBarrier();
            InterlockedExchangePointer((PVOID*)&victim->string, dup);
            return;
        }
    }
}

#define TLS_FILENAME_CAP 2048u

typedef struct {
    char      lower_buf[TLS_FILENAME_CAP];
    uint32_t  trigram_buf[TLS_FILENAME_CAP];
} BloomThreadBuffers;

typedef struct {
    const char* original;
    size_t      original_len;
    char*       lower;
    size_t      lower_len;
    BOOL        lower_heap;
    uint32_t*   trigrams;
    size_t      trigram_count;
    BOOL        trigrams_heap;
    uint32_t    hash_count;
    uint32_t    bloom_bytes;
    uint32_t    bloom_mask;
} NameBloomContext;

static THREAD_LOCAL BloomThreadBuffers g_bloom_tls_buffers;

static void name_bloom_context_init(NameBloomContext* ctx){
    if(!ctx) return;
    ctx->original = NULL;
    ctx->original_len = 0;
    ctx->lower = NULL;
    ctx->lower_len = 0;
    ctx->lower_heap = FALSE;
    ctx->trigrams = NULL;
    ctx->trigram_count = 0;
    ctx->trigrams_heap = FALSE;
    ctx->hash_count = 0;
    ctx->bloom_bytes = 0;
    ctx->bloom_mask = 0;
}

static size_t fill_trigram_values(const char* text, size_t len, uint32_t* out){
    if(len < 3 || !text || !out) return 0;
    size_t tri_count = len - 2;
#if defined(__SSE4_1__)
    size_t i = 0;
    if(tri_count >= 4){
        size_t simd_limit = tri_count & ~3ull;
        for(; i < simd_limit; i += 4){
            __m128i bytes = _mm_loadu_si128((const __m128i*)(text + i));
            __m128i b0 = _mm_cvtepu8_epi32(bytes);
            __m128i b1 = _mm_cvtepu8_epi32(_mm_srli_si128(bytes,1));
            __m128i b2 = _mm_cvtepu8_epi32(_mm_srli_si128(bytes,2));
            __m128i t  = _mm_or_si128(_mm_slli_epi32(b0,16),
                                _mm_or_si128(_mm_slli_epi32(b1,8), b2));
            _mm_storeu_si128((__m128i*)(out + i), t);
        }
    }
    for(; i < tri_count; ++i){
        out[i] = ((uint32_t)(uint8_t)text[i] << 16) |
                 ((uint32_t)(uint8_t)text[i+1] << 8) |
                 (uint32_t)(uint8_t)text[i+2];
    }
#else
    for(size_t i=0;i<tri_count;i++){
        out[i] = ((uint32_t)(uint8_t)text[i] << 16) |
                 ((uint32_t)(uint8_t)text[i+1] << 8) |
                 (uint32_t)(uint8_t)text[i+2];
    }
#endif
    return tri_count;
}

static BOOL name_bloom_context_prepare(const char* name_u8, NameBloomContext* ctx){
    if(!name_u8 || !ctx) return FALSE;
    name_bloom_context_init(ctx);
    size_t len = strlen(name_u8);
    ctx->original = name_u8;
    ctx->original_len = len;
    if(len == 0){
        ctx->lower = g_bloom_tls_buffers.lower_buf;
        ctx->trigrams = g_bloom_tls_buffers.trigram_buf;
        ctx->lower_len = 0;
        ctx->trigram_count = 0;
        return TRUE;
    }

    BOOL use_tls_lower = (len + 1) <= TLS_FILENAME_CAP;
    char* lower = use_tls_lower ? g_bloom_tls_buffers.lower_buf : (char*)malloc(len + 1);
    if(!lower) return FALSE;
    memcpy(lower, name_u8, len + 1);
    lowercase_ascii(lower, len);
    ctx->lower = lower;
    ctx->lower_len = len;
    ctx->lower_heap = !use_tls_lower;

    size_t tri_count = len >= 3 ? (len - 2) : 0;
    BOOL use_tls_tris = tri_count <= TLS_FILENAME_CAP;
    uint32_t* tris = tri_count ? (use_tls_tris ? g_bloom_tls_buffers.trigram_buf : (uint32_t*)malloc(tri_count * sizeof(uint32_t))) : g_bloom_tls_buffers.trigram_buf;
    if(tri_count && !tris){
        if(!use_tls_lower) free(lower);
        name_bloom_context_init(ctx);
        return FALSE;
    }
    if(tri_count){
        fill_trigram_values(lower, len, tris);
    }
    ctx->trigrams = tris;
    ctx->trigram_count = tri_count;
    ctx->trigrams_heap = tri_count && !use_tls_tris;
    return TRUE;
}

static void name_bloom_context_release(NameBloomContext* ctx){
    if(!ctx) return;
    if(ctx->lower_heap && ctx->lower){
        free(ctx->lower);
    }
    if(ctx->trigrams_heap && ctx->trigrams){
        free(ctx->trigrams);
    }
    name_bloom_context_init(ctx);
}

static inline uint8_t bloom_bytes_to_log2(size_t bytes){
    switch(bytes){
        case 2048: return 11;
        case 4096: return 12;
        case 8192: return 13;
        default:   return 0;
    }
}

static inline size_t bloom_log2_to_bytes(uint8_t log2){
    if(log2 >= 8 && log2 <= 20){
        return (size_t)1u << log2;
    }
    return 0;
}

static void string_meta_init(StringMeta* sm){
    if(!sm) return;
    ZeroMemory(sm, sizeof(*sm));
    sm->magic0 = STRING_META_MAGIC0;
    sm->magic1 = STRING_META_MAGIC1;
}

static BOOL string_value_parse(const MDB_val* value, MDB_val* text, StringMeta* meta_out, BOOL* has_meta){
    if(!value) return FALSE;
    BOOL present = FALSE;
    size_t total = value->mv_size;
    if(total >= sizeof(StringMeta)){
        const uint8_t* base = (const uint8_t*)value->mv_data;
        const StringMeta* tail = (const StringMeta*)(base + total - sizeof(StringMeta));
        if(tail->magic0 == STRING_META_MAGIC0 && tail->magic1 == STRING_META_MAGIC1){
            present = TRUE;
            if(meta_out) *meta_out = *tail;
            total -= sizeof(StringMeta);
        }
    }
    if(meta_out && !present){
        ZeroMemory(meta_out, sizeof(*meta_out));
    }
    if(text){
        text->mv_data = value->mv_data;
        text->mv_size = total;
    }
    if(has_meta) *has_meta = present;
    return present;
}

static BOOL string_value_update(DbImpl* d, uint64_t id, const MDB_val* current, const StringMeta* meta){
    if(!d || !current || !meta || !d->wtxn) return FALSE;
    MDB_val text;
    string_value_parse(current, &text, NULL, NULL);
    size_t text_len = text.mv_size;
    size_t total = text_len + sizeof(StringMeta);
    uint8_t* buf = (uint8_t*)malloc(total);
    if(!buf) return FALSE;
    memcpy(buf, text.mv_data, text_len);
    memcpy(buf + text_len, meta, sizeof(StringMeta));
    MDB_val key = {.mv_data=&id,.mv_size=sizeof(id)};
    MDB_val val = {.mv_data=buf,.mv_size=total};
    int rc = mdb_put(d->wtxn, d->dbi_strings, &key, &val, 0);
    free(buf);
    if(rc) return FALSE;
    return TRUE;
}

static BOOL bloom_buffer_ensure(DbImpl* d, size_t additional){
    if(!d) return FALSE;
    size_t needed = d->bloom_buffer_len + additional;
    if(needed <= d->bloom_buffer_cap) return TRUE;
    size_t newcap = d->bloom_buffer_cap ? d->bloom_buffer_cap : BLOOM_BUFFER_CHUNK;
    while(newcap < needed){
        newcap += BLOOM_BUFFER_CHUNK;
    }
    uint8_t* tmp = (uint8_t*)realloc(d->bloom_buffer, newcap);
    if(!tmp){
        set_error(d, DB_ERROR_OS, 0, "out of memory");
        return FALSE;
    }
    d->bloom_buffer = tmp;
    d->bloom_buffer_cap = newcap;
    return TRUE;
}

static BOOL bloom_buffer_append(DbImpl* d, const uint8_t* data, size_t size){
    if(!d || !data || size == 0) return TRUE;
    if(!bloom_buffer_ensure(d, size)) return FALSE;
    memcpy(d->bloom_buffer + d->bloom_buffer_len, data, size);
    d->bloom_buffer_len += size;
    d->bloom_offset = d->bloom_file_size + d->bloom_buffer_len;
    return TRUE;
}

static BOOL bloom_buffer_flush(DbImpl* d){
    if(!d || !d->bloom_file) return FALSE;
    if(d->bloom_buffer_len == 0) return TRUE;
    LARGE_INTEGER li; li.QuadPart = (LONGLONG)d->bloom_file_size;
    if(!SetFilePointerEx(d->bloom_file, li, NULL, FILE_BEGIN)){
        set_sys_error(d, GetLastError());
        return FALSE;
    }
    size_t written = 0;
    uint64_t original_size = d->bloom_file_size;
    while(written < d->bloom_buffer_len){
        size_t remaining = d->bloom_buffer_len - written;
        DWORD chunk = (DWORD)((remaining < BLOOM_BUFFER_CHUNK) ? remaining : BLOOM_BUFFER_CHUNK);
        DWORD wr = 0;
        if(!WriteFile(d->bloom_file, d->bloom_buffer + written, chunk, &wr, NULL) || wr != chunk){
            set_sys_error(d, GetLastError());
            LARGE_INTEGER rollback; rollback.QuadPart = (LONGLONG)original_size;
            SetFilePointerEx(d->bloom_file, rollback, NULL, FILE_BEGIN);
            SetEndOfFile(d->bloom_file);
            return FALSE;
        }
        written += wr;
        d->bloom_file_size += wr;
    }
    d->bloom_buffer_len = 0;
    d->bloom_offset = d->bloom_file_size;
    return TRUE;
}

static BOOL ensure_bloom_for_string(DbImpl* d, uint64_t str_id, MDB_val* value, NameBloomContext* ctx, StringMeta* existing_meta, uint64_t* bloom_offset_tmp){
    if(!d || !value || !ctx || !bloom_offset_tmp) return FALSE;
    BOOL meta_present = existing_meta && existing_meta->magic0 == STRING_META_MAGIC0 && existing_meta->magic1 == STRING_META_MAGIC1;
    BOOL has_bloom = meta_present && existing_meta->hash_count > 0 && existing_meta->bloom_length > 0;

    if(ctx->lower_len < 5){
        if(!meta_present || existing_meta->hash_count != 0 || existing_meta->bloom_length != 0 || existing_meta->bloom_offset != 0 || existing_meta->bloom_log2 != 0 || existing_meta->trigram_count != (uint32_t)ctx->trigram_count){
            StringMeta sm; string_meta_init(&sm);
            sm.trigram_count = (uint32_t)ctx->trigram_count;
            if(!string_value_update(d, str_id, value, &sm)){
                set_error(d, DB_ERROR_OS, 0, "failed to update string meta");
                return FALSE;
            }
            if(existing_meta) *existing_meta = sm;
        }
        return TRUE;
    }

    if(has_bloom){
        return TRUE;
    }

    if(ctx->bloom_bytes > 8192u){
        set_error(d, DB_ERROR_OS, 0, "unsupported bloom size");
        return FALSE;
    }

    uint8_t bloom[8192];
    uint32_t tc = build_bloom_from_context(ctx, bloom);
    if(ctx->bloom_bytes == 0 || ctx->hash_count == 0 || tc == 0){
        StringMeta sm; string_meta_init(&sm);
        sm.trigram_count = tc ? tc : (uint32_t)ctx->trigram_count;
        if(!string_value_update(d, str_id, value, &sm)){
            set_error(d, DB_ERROR_OS, 0, "failed to update string meta");
            return FALSE;
        }
        if(existing_meta) *existing_meta = sm;
        return TRUE;
    }

    uint8_t encoded[8192 + 1024];
    size_t bloom_bytes = ctx->bloom_bytes;
    size_t enc_len = bloom_packbits_compress(bloom, bloom_bytes, encoded, sizeof(encoded));
    const uint8_t* out_data = bloom;
    DWORD out_bytes = (DWORD)bloom_bytes;
    uint32_t stored_len = (uint32_t)bloom_bytes;
    if(enc_len > 0 && enc_len < bloom_bytes){
        out_data = encoded;
        out_bytes = (DWORD)enc_len;
        stored_len = (uint32_t)enc_len;
    }
    if(*bloom_offset_tmp > UINT64_MAX - stored_len){
        set_error(d, DB_ERROR_OS, 0, "bloom file too large");
        return FALSE;
    }
    if(!bloom_buffer_append(d, out_data, out_bytes)){
        return FALSE;
    }
    StringMeta sm; string_meta_init(&sm);
    sm.trigram_count = tc;
    sm.hash_count = ctx->hash_count;
    sm.bloom_log2 = bloom_bytes_to_log2(bloom_bytes);
    sm.bloom_offset = *bloom_offset_tmp;
    sm.bloom_length = stored_len;
    if(!string_value_update(d, str_id, value, &sm)){
        set_error(d, DB_ERROR_OS, 0, "failed to update string meta");
        return FALSE;
    }
    *bloom_offset_tmp += stored_len;
    if(existing_meta) *existing_meta = sm;
    return TRUE;
}

static BOOL append_index64(IndexEntry64** entries, size_t* count, size_t* cap, uint64_t key, uint64_t value){
    if(!entries || !count || !cap) return FALSE;
    if(*count == *cap){
        size_t newcap = *cap ? (*cap * 2) : 64;
        IndexEntry64* tmp = (IndexEntry64*)realloc(*entries, newcap * sizeof(IndexEntry64));
        if(!tmp) return FALSE;
        *entries = tmp;
        *cap = newcap;
    }
    (*entries)[*count].key = key;
    (*entries)[*count].value = value;
    (*count)++;
    return TRUE;
}

static BOOL append_index32(IndexEntry32** entries, size_t* count, size_t* cap, uint32_t key, uint64_t value){
    if(!entries || !count || !cap) return FALSE;
    if(*count == *cap){
        size_t newcap = *cap ? (*cap * 2) : 64;
        IndexEntry32* tmp = (IndexEntry32*)realloc(*entries, newcap * sizeof(IndexEntry32));
        if(!tmp) return FALSE;
        *entries = tmp;
        *cap = newcap;
    }
    (*entries)[*count].key = key;
    (*entries)[*count].value = value;
    (*count)++;
    return TRUE;
}

static BOOL append_extension_entry(ExtensionEntry** entries, size_t* count, size_t* cap, const char* key, size_t len, uint64_t value){
    if(!entries || !count || !cap || !key || len == 0) return TRUE;
    if(len >= sizeof((*entries)[0].key)) len = sizeof((*entries)[0].key) - 1;
    if(*count == *cap){
        size_t newcap = *cap ? (*cap * 2) : 32;
        ExtensionEntry* tmp = (ExtensionEntry*)realloc(*entries, newcap * sizeof(ExtensionEntry));
        if(!tmp) return FALSE;
        *entries = tmp;
        *cap = newcap;
    }
    ExtensionEntry* e = &(*entries)[*count];
    memcpy(e->key, key, len);
    e->key[len] = 0;
    e->len = (uint8_t)len;
    e->value = value;
    (*count)++;
    return TRUE;
}

static const uint32_t kBloomHashSeeds[4] = {
    0xA5A5A5A5u, 0x3C3C3C3Cu, 0x5A5A5A5Au, 0x1F1F1F1Fu
};

#define BLOOM_HASH_CACHE_SIZE 64u
#define BLOOM_HASH_BATCH      16u

typedef struct {
    uint32_t trigram;
    uint32_t hashes[4];
    uint64_t tick;
    BOOL     filled;
} BloomHashCacheEntry;

static BloomHashCacheEntry g_bloom_hash_cache[BLOOM_HASH_CACHE_SIZE];
static uint64_t g_bloom_hash_tick = 1;

static BloomHashCacheEntry* bloom_hash_cache_lookup(uint32_t trigram){
    BloomHashCacheEntry* oldest = NULL;
    uint64_t oldest_tick = UINT64_MAX;
    for(size_t i=0;i<BLOOM_HASH_CACHE_SIZE;i++){
        BloomHashCacheEntry* e = &g_bloom_hash_cache[i];
        if(e->filled && e->trigram == trigram){
            e->tick = g_bloom_hash_tick++;
            return e;
        }
        if(!e->filled){
            oldest = e;
            oldest_tick = 0;
        } else if(e->tick < oldest_tick){
            oldest_tick = e->tick;
            oldest = e;
        }
    }
    return oldest;
}

static BloomHashCacheEntry* bloom_hash_cache_reserve(uint32_t trigram){
    BloomHashCacheEntry* slot = bloom_hash_cache_lookup(trigram);
    if(slot && slot->filled && slot->trigram == trigram){
        return slot;
    }
    if(!slot){
        slot = &g_bloom_hash_cache[0];
    }
    slot->filled = FALSE;
    slot->trigram = trigram;
    slot->tick = g_bloom_hash_tick++;
    return slot;
}

static inline uint32_t hash_trigram_scalar(uint32_t trigram, uint32_t seed){
    uint32_t h = trigram ^ seed;
    h *= 16777619u;
    return h;
}

static void compute_hashes_batch(const uint32_t* tris, size_t count, uint32_t hash_count, uint32_t* out){
    if(count == 0 || hash_count == 0) return;
#if defined(__AVX2__)
    size_t i = 0;
    __m256i prime = _mm256_set1_epi32(16777619);
    for(; i + 8 <= count; i += 8){
        __m256i tri = _mm256_loadu_si256((const __m256i*)(tris + i));
        for(uint32_t s=0; s<hash_count; ++s){
            __m256i seed = _mm256_set1_epi32((int)kBloomHashSeeds[s]);
            __m256i hashed = _mm256_mullo_epi32(_mm256_xor_si256(tri, seed), prime);
            _mm256_storeu_si256((__m256i*)(out + s*count + i), hashed);
        }
    }
    for(; i < count; ++i){
        uint32_t t = tris[i];
        for(uint32_t s=0; s<hash_count; ++s){
            out[s*count + i] = hash_trigram_scalar(t, kBloomHashSeeds[s]);
        }
    }
#else
    for(size_t i=0;i<count;i++){
        uint32_t t = tris[i];
        for(uint32_t s=0; s<hash_count; ++s){
            out[s*count + i] = hash_trigram_scalar(t, kBloomHashSeeds[s]);
        }
    }
#endif
}

static void flush_pending_hashes(uint8_t* bloom, uint32_t actual_hashes,
                                 uint32_t pending_count,
                                 const uint32_t* pending_tris,
                                 BloomHashCacheEntry** pending_entries,
                                 uint32_t bloom_mask){
    if(pending_count == 0) return;
    uint32_t results[4 * BLOOM_HASH_BATCH];
    compute_hashes_batch(pending_tris, pending_count, 4, results);
    for(uint32_t i=0;i<pending_count;i++){
        BloomHashCacheEntry* entry = pending_entries[i];
        if(!entry) continue;
        entry->trigram = pending_tris[i];
        entry->tick = g_bloom_hash_tick++;
        entry->filled = TRUE;
        for(uint32_t s=0;s<4;s++){
            entry->hashes[s] = results[s * pending_count + i];
        }
        for(uint32_t s=0;s<actual_hashes && s<4;s++){
            bloom_set(bloom, entry->hashes[s], bloom_mask);
        }
    }
}

static void queue_trigram_for_bloom(uint32_t trigram,
                                    uint8_t* bloom,
                                    uint32_t hash_count,
                                    uint32_t* pending_tris,
                                    BloomHashCacheEntry** pending_entries,
                                    uint32_t* pending_n,
                                    uint32_t bloom_mask){
    BloomHashCacheEntry* entry = bloom_hash_cache_lookup(trigram);
    if(entry && entry->filled && entry->trigram == trigram){
        entry->tick = g_bloom_hash_tick++;
        for(uint32_t s=0; s<hash_count && s<4; ++s){
            bloom_set(bloom, entry->hashes[s], bloom_mask);
        }
    } else {
        entry = bloom_hash_cache_reserve(trigram);
        pending_tris[*pending_n] = trigram;
        pending_entries[*pending_n] = entry;
        (*pending_n)++;
        if(*pending_n == BLOOM_HASH_BATCH){
            flush_pending_hashes(bloom, hash_count, *pending_n, pending_tris, pending_entries, bloom_mask);
            *pending_n = 0;
        }
    }
}

static size_t bloom_packbits_compress(const uint8_t* src, size_t len, uint8_t* dst, size_t dst_cap){
    if(!src || !dst || dst_cap == 0) return 0;
    size_t i = 0;
    size_t o = 0;
    while(i < len){
        size_t run = 1;
        while(run < 128 && i + run < len && src[i] == src[i + run]){
            run++;
        }
        if(run >= 3){
            if(o + 2 > dst_cap) return 0;
            dst[o++] = (uint8_t)(1 - (int)run);
            dst[o++] = src[i];
            i += run;
        } else {
            size_t lit_start = i;
            size_t lit_len = 0;
            while(i < len){
                run = 1;
                while(run < 128 && i + run < len && src[i] == src[i + run]){
                    run++;
                }
                if(run >= 3){
                    break;
                }
                size_t copy = run;
                if(lit_len + copy > 128){
                    copy = 128 - lit_len;
                }
                i += copy;
                lit_len += copy;
                if(lit_len == 128){
                    break;
                }
            }
            if(lit_len == 0){
                lit_len = run;
                i += run;
            }
            if(o + 1 + lit_len > dst_cap) return 0;
            dst[o++] = (uint8_t)(lit_len - 1);
            memcpy(dst + o, src + lit_start, lit_len);
            o += lit_len;
        }
    }
    return o;
}

static uint32_t build_bloom_from_context(NameBloomContext* ctx, uint8_t* bloom){
    if(!ctx || !bloom) return 0;
    ctx->hash_count = 0;
    ctx->bloom_bytes = 0;
    ctx->bloom_mask = 0;
    if(!ctx->lower || ctx->lower_len < 3 || !ctx->trigrams){
        return 0;
    }

    if(ctx->lower_len < 5){
        return 0;
    }

    size_t bloom_bytes = 8192;
    if(ctx->lower_len < 32){
        bloom_bytes = 2048;
    } else if(ctx->lower_len < 64){
        bloom_bytes = 4096;
    }
    ZeroMemory(bloom, bloom_bytes);
    ctx->bloom_bytes = (uint32_t)bloom_bytes;
    ctx->bloom_mask = (uint32_t)(bloom_bytes * 8 - 1);

    if(ctx->lower_len < 10){
        ctx->hash_count = 1;
    } else if(ctx->lower_len < 30){
        ctx->hash_count = 2;
    } else if(ctx->lower_len < 100){
        ctx->hash_count = 3;
    } else {
        ctx->hash_count = 4;
    }

    size_t limit = ctx->lower_len > DB_BLOOM_MAX_BYTES ? DB_BLOOM_MAX_BYTES : ctx->lower_len;
    size_t full = limit < DB_BLOOM_STRIDE_AFTER ? limit : DB_BLOOM_STRIDE_AFTER;
    size_t tri_full = full >= 3 ? (full - 2) : 0;
    size_t tri_sampled = 0;
    if(limit > full){
        size_t rem = limit - full;
        if(rem > 3) tri_sampled = ((rem - 3) / DB_BLOOM_STRIDE) + 1;
    }
    size_t tri_est = tri_full + tri_sampled;
    (void)tri_est;

    uint32_t tcount = 0;
    uint32_t pending_tris[BLOOM_HASH_BATCH];
    BloomHashCacheEntry* pending_entries[BLOOM_HASH_BATCH];
    uint32_t pending_n = 0;

    for(size_t i=0;i<tri_full;i++){
        queue_trigram_for_bloom(ctx->trigrams[i], bloom, ctx->hash_count, pending_tris, pending_entries, &pending_n, ctx->bloom_mask);
        tcount++;
    }
    for(size_t i=full; i + 3 <= limit; i += DB_BLOOM_STRIDE){
        size_t idx = i;
        if(idx < ctx->trigram_count){
            queue_trigram_for_bloom(ctx->trigrams[idx], bloom, ctx->hash_count, pending_tris, pending_entries, &pending_n, ctx->bloom_mask);
            tcount++;
        }
    }
    if(pending_n){
        flush_pending_hashes(bloom, ctx->hash_count, pending_n, pending_tris, pending_entries, ctx->bloom_mask);
    }
    return tcount;
}

static inline void bloom_set(uint8_t* bloom, uint32_t h, uint32_t mask){
    if(mask == 0) return;
    uint32_t bit = h & mask;
    bloom[bit>>3] |= (uint8_t)(1u << (bit & 7));
}
static inline BOOL bloom_has(const uint8_t* bloom, uint32_t h, uint32_t mask){
    if(mask == 0) return FALSE;
    uint32_t bit = h & mask;
    return (bloom[bit>>3] & (uint8_t)(1u << (bit & 7))) != 0;
}
static uint32_t hash32_seed(const void* data, size_t len, uint32_t seed){
    const uint8_t* p=(const uint8_t*)data;
    uint32_t h=2166136261u ^ seed;
    for(size_t i=0;i<len;i++){ h ^= p[i]; h *= 16777619u; }
    return h;
}

void build_bloom_hashes_simd(const char* tri, uint32_t* out4){
    __m128i seeds = _mm_set_epi32(0x1F1F1F1F, 0x5A5A5A5A, 0x3C3C3C3C, 0xA5A5A5A5);
    uint32_t t= (uint8_t)tri[0] | ((uint32_t)(uint8_t)tri[1]<<8) | ((uint32_t)(uint8_t)tri[2]<<16);
    __m128i data = _mm_set1_epi32((int)t);
    __m128i x = _mm_xor_si128(data, seeds);
    __m128i mul = _mm_set1_epi32(16777619);
    __m128i res = _mm_mullo_epi32(x, mul);
    _mm_storeu_si128((__m128i*)out4, res);
}
uint32_t build_bloom_for_name(const char* name_u8, uint8_t* bloom){
    if(!name_u8 || !name_u8[0] || !bloom) return 0;
    NameBloomContext ctx;
    if(!name_bloom_context_prepare(name_u8, &ctx)) return 0;
    uint32_t tcount = build_bloom_from_context(&ctx, bloom);
    name_bloom_context_release(&ctx);
    return tcount;
}


#define ALIGN_UP(x,a)   (((x)+(a)-1) & ~((a)-1))

typedef struct {
    MDB_env* env;
    MDB_dbi  dbi_meta;
    MDB_dbi  dbi_strings;
    MDB_dbi  dbi_strrev;
    MDB_dbi  dbi_records;
    MDB_dbi  dbi_fname_index;
    MDB_dbi  dbi_parent_index;
    MDB_dbi  dbi_size_index;      // key: uint64 size        → rec_id (dups)
    MDB_dbi  dbi_date_index;      // key: uint64 day         → rec_id (dups)
    MDB_dbi  dbi_mtime_index;     // key: uint64 modified    → rec_id (dups)
    MDB_dbi  dbi_attr_index;      // key: uint32 attributes  → rec_id (dups)
    MDB_dbi  dbi_extension_index; // key: utf8 ext   → rec_id (dups)
    MDB_dbi  dbi_path_hierarchy;  // key: parent_id  → rec_id (dups)
    MDB_dbi  dbi_trigram_index;   // key: 3 bytes    → string_id (dups)
    MDB_dbi  dbi_content_index;  // key: content_str_id → rec_id (dups)
    MDB_dbi  dbi_author_index;   // key: author_str_id  → rec_id (dups)
    MDB_dbi  dbi_camera_index;   // key: camera_str_id  → rec_id (dups)
    MDB_dbi  dbi_lens_index;     // key: lens_str_id    → rec_id (dups)
    MDB_dbi  dbi_artist_index;   // key: artist_str_id  → rec_id (dups)
    MDB_dbi  dbi_album_index;    // key: album_str_id   → rec_id (dups)
    MDB_dbi  dbi_title_index;    // key: title_str_id   → rec_id (dups)
    MDB_txn* wtxn;
    DbError  last_error;
    size_t   map_init;
    size_t   map_max;
    HANDLE   bloom_file;
    uint8_t* bloom_buffer;
    size_t   bloom_buffer_len;
    size_t   bloom_buffer_cap;
    uint64_t bloom_file_size;
    uint64_t bloom_offset;
    BOOL     dirty;
    DbHeader header_cache;
    IndexLoadState load_state;
    size_t   last_write_progress;
} DbImpl;

static const size_t BLOOM_BUFFER_CHUNK = 1u << 20; // 1 MB

typedef struct { uint64_t key; uint64_t value; } IndexEntry64;
typedef struct { uint32_t key; uint64_t value; } IndexEntry32;
typedef struct { char key[32]; uint8_t len; uint64_t value; } ExtensionEntry;

static void set_error(DbImpl* d, DbErrorCode code, int detail, const char* msg){
    if(!d) return;
    d->last_error.code = code;
    d->last_error.detail = detail;
    if(msg){
        snprintf(d->last_error.message, sizeof(d->last_error.message), "%s", msg);
    } else {
        d->last_error.message[0] = '\0';
    }
}
static void set_mdb_error(DbImpl* d, int rc){
    set_error(d, DB_ERROR_LMDB, rc, mdb_strerror(rc));
}
static void set_sys_error(DbImpl* d, DWORD err){
    char buf[256];
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,NULL,err,0,buf,sizeof(buf),NULL);
    set_error(d, DB_ERROR_OS, (int)err, buf);
}
static uint64_t now_filetime(void) {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u; u.LowPart=ft.dwLowDateTime; u.HighPart=ft.dwHighDateTime;
    return u.QuadPart;
}
static void to_mdb_val(const void* p, size_t n, MDB_val* mv){ mv->mv_data=(void*)p; mv->mv_size=(size_t)n; }

static BOOL ensure_dir(const wchar_t* path){
    wchar_t tmp[MAX_PATH]; wcsncpy_s(tmp, MAX_PATH, path, _TRUNCATE);
    wchar_t* p = wcsrchr(tmp, L'\\'); if(p){ *p=0; CreateDirectoryW(tmp, NULL); }
    return TRUE;
}

static int open_core_dbs(MDB_txn* txn, DbImpl* d, BOOL create){
    unsigned int flags = create? MDB_CREATE:0;
    int rc = mdb_dbi_open(txn, "meta", flags, &d->dbi_meta);
    if(rc) return rc;
    if((rc = mdb_dbi_open(txn, "strings", flags, &d->dbi_strings))) return rc;
    if((rc = mdb_dbi_open(txn, "strrev", flags, &d->dbi_strrev))) return rc;
    if((rc = mdb_dbi_open(txn, "records", flags, &d->dbi_records))) return rc;
    if((rc = mdb_dbi_open(txn, "filename_index", flags|MDB_DUPSORT, &d->dbi_fname_index))) return rc;
    if((rc = mdb_dbi_open(txn, "parent_index", flags|MDB_DUPSORT, &d->dbi_parent_index))) return rc;
    return 0;
}

static int open_metadata_dbs(MDB_txn* txn, DbImpl* d, BOOL create){
    unsigned int flags = create? MDB_CREATE:0;
    int rc;
    if((rc = mdb_dbi_open(txn, "size_index", flags|MDB_DUPSORT|MDB_INTEGERKEY, &d->dbi_size_index))) return rc;
    if((rc = mdb_dbi_open(txn, "date_index", flags|MDB_DUPSORT|MDB_INTEGERKEY, &d->dbi_date_index))) return rc;
    if((rc = mdb_dbi_open(txn, "mtime_index", flags|MDB_DUPSORT|MDB_INTEGERKEY, &d->dbi_mtime_index))) return rc;
    if((rc = mdb_dbi_open(txn, "attr_index", flags|MDB_DUPSORT|MDB_INTEGERKEY, &d->dbi_attr_index))) return rc;
    if((rc = mdb_dbi_open(txn, "extension_index", flags|MDB_DUPSORT, &d->dbi_extension_index))) return rc;
    if((rc = mdb_dbi_open(txn, "path_hierarchy", flags|MDB_DUPSORT, &d->dbi_path_hierarchy))) return rc;
    return 0;
}

static int open_content_dbs(MDB_txn* txn, DbImpl* d, BOOL create){
    unsigned int flags = create? MDB_CREATE:0;
    int rc;
    if((rc = mdb_dbi_open(txn, "trigram_index", flags|MDB_DUPSORT, &d->dbi_trigram_index))) return rc;
    if((rc = mdb_dbi_open(txn, "content_index", flags|MDB_DUPSORT, &d->dbi_content_index))) return rc;
    if((rc = mdb_dbi_open(txn, "author_index", flags|MDB_DUPSORT, &d->dbi_author_index))) return rc;
    if((rc = mdb_dbi_open(txn, "camera_index", flags|MDB_DUPSORT, &d->dbi_camera_index))) return rc;
    if((rc = mdb_dbi_open(txn, "lens_index", flags|MDB_DUPSORT, &d->dbi_lens_index))) return rc;
    if((rc = mdb_dbi_open(txn, "artist_index", flags|MDB_DUPSORT, &d->dbi_artist_index))) return rc;
    if((rc = mdb_dbi_open(txn, "album_index", flags|MDB_DUPSORT, &d->dbi_album_index))) return rc;
    if((rc = mdb_dbi_open(txn, "title_index", flags|MDB_DUPSORT, &d->dbi_title_index))) return rc;
    return 0;
}

static int ensure_indices(DbImpl* d, IndexLoadState desired){
    if(d->load_state >= desired) return 0;
    MDB_txn* txn;
    int rc = mdb_txn_begin(d->env, NULL, MDB_RDONLY, &txn);
    if(rc) return rc;
    if(d->load_state < INDEX_METADATA_LOADED && desired >= INDEX_METADATA_LOADED){
        rc = open_metadata_dbs(txn, d, FALSE);
        if(rc){ mdb_txn_abort(txn); return rc; }
        d->load_state = INDEX_METADATA_LOADED;
    }
    if(d->load_state < INDEX_CONTENT_LOADED && desired >= INDEX_CONTENT_LOADED){
        rc = open_content_dbs(txn, d, FALSE);
        if(rc){ mdb_txn_abort(txn); return rc; }
        d->load_state = INDEX_CONTENT_LOADED;
    }
    mdb_txn_abort(txn);
    return 0;
}

BOOL db_create(const wchar_t* path, size_t map_init_mb, size_t map_max_mb, Db** out_db){
    if(!out_db){ return FALSE; }
    *out_db=NULL;
    DbImpl* d = (DbImpl*)calloc(1,sizeof(DbImpl));
    if(!d){ return FALSE; }
    d->map_init = (size_t)map_init_mb * 1024ull * 1024ull;
    d->map_max  = (size_t)map_max_mb  * 1024ull * 1024ull;
    size_t min_init = 1024ull * 1024ull * 1024ull; // start with at least 1 GB
    if(d->map_init < min_init) d->map_init = min_init;
    if(d->map_max  < d->map_init)   d->map_max = d->map_init*4;
    ensure_dir(path);
    int rc = mdb_env_create(&d->env);
    if(rc){ set_mdb_error(d,rc); free(d); return FALSE; }
    mdb_env_set_mapsize(d->env, d->map_init);
    mdb_env_set_maxdbs(d->env, 64);
    char u8[MAX_PATH*3]; WideCharToMultiByte(CP_UTF8,0,path,-1,u8,sizeof(u8),NULL,NULL);
    rc = mdb_env_open(d->env, u8, MDB_WRITEMAP|MDB_MAPASYNC, 0664);
    if(rc){ set_mdb_error(d,rc); mdb_env_close(d->env); free(d); return FALSE; }
    wchar_t bloomPath[MAX_LONG_PATH];
    swprintf(bloomPath, MAX_LONG_PATH, L"%s\\bloom.dat", path);
    wchar_t lp[MAX_LONG_PATH]; make_long_path(bloomPath, lp, MAX_LONG_PATH);
    d->bloom_file = CreateFileW(lp, GENERIC_READ|GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if(d->bloom_file==INVALID_HANDLE_VALUE){ set_sys_error(d, GetLastError()); mdb_env_close(d->env); free(d); return FALSE; }
    LARGE_INTEGER sz; sz.QuadPart = 0; GetFileSizeEx(d->bloom_file, &sz); d->bloom_offset = sz.QuadPart; SetFilePointerEx(d->bloom_file, sz, NULL, FILE_BEGIN);
    d->bloom_file_size = d->bloom_offset;
    d->bloom_buffer = (uint8_t*)malloc(BLOOM_BUFFER_CHUNK);
    if(!d->bloom_buffer){ CloseHandle(d->bloom_file); mdb_env_close(d->env); free(d); return FALSE; }
    d->bloom_buffer_cap = BLOOM_BUFFER_CHUNK;
    d->bloom_buffer_len = 0;
    MDB_txn* txn;
    if((rc = mdb_txn_begin(d->env, NULL, 0, &txn))){ set_mdb_error(d,rc); mdb_env_close(d->env); free(d); return FALSE; }
    if((rc = open_core_dbs(txn, d, TRUE))){ set_mdb_error(d,rc); mdb_txn_abort(txn); mdb_env_close(d->env); free(d); return FALSE; }
    if((rc = open_metadata_dbs(txn, d, TRUE))){ set_mdb_error(d,rc); mdb_txn_abort(txn); mdb_env_close(d->env); free(d); return FALSE; }
    if((rc = open_content_dbs(txn, d, TRUE))){ set_mdb_error(d,rc); mdb_txn_abort(txn); mdb_env_close(d->env); free(d); return FALSE; }
    // initialize header
    DbHeader hdr = {0};
    hdr.created_time = hdr.updated_time = now_filetime();
    hdr.map_size_bytes = d->map_init;
    hdr.generation = 0;
    hdr.journal_seq = 0;
    MDB_val k,v; const char* H="header"; to_mdb_val(H, strlen(H), &k); to_mdb_val(&hdr, sizeof(hdr), &v);
    if((rc = mdb_put(txn, d->dbi_meta, &k, &v, 0))){ set_mdb_error(d,rc); mdb_txn_abort(txn); mdb_env_close(d->env); free(d); return FALSE; }
    if((rc = mdb_txn_commit(txn))){ set_mdb_error(d,rc); mdb_env_close(d->env); free(d); return FALSE; }
    d->header_cache = hdr;
    d->load_state = INDEX_CONTENT_LOADED;
    *out_db = (Db*)d;
    return TRUE;
}

const DbHeader* db_open_readonly(const wchar_t* path, Db** out_db){
    if(!out_db){ return NULL; }
    *out_db=NULL;
    DbImpl* d = (DbImpl*)calloc(1,sizeof(DbImpl));
    if(!d){ return NULL; }
    d->map_init = 128*1024*1024; d->map_max = d->map_init*16;
    int rc = mdb_env_create(&d->env);
    if(rc){ set_mdb_error(d,rc); free(d); return NULL; }
    mdb_env_set_maxdbs(d->env, 64);
    char u8[MAX_PATH*3]; WideCharToMultiByte(CP_UTF8,0,path,-1,u8,sizeof(u8),NULL,NULL);
    if((rc = mdb_env_open(d->env, u8, MDB_RDONLY, 0664))){ set_mdb_error(d,rc); mdb_env_close(d->env); free(d); return NULL; }
    MDB_txn* txn;
    if((rc = mdb_txn_begin(d->env, NULL, MDB_RDONLY, &txn))){ set_mdb_error(d,rc); mdb_env_close(d->env); free(d); return NULL; }
    if((rc = open_core_dbs(txn, d, FALSE))){ set_mdb_error(d,rc); mdb_txn_abort(txn); mdb_env_close(d->env); free(d); return NULL; }
    // read header
    MDB_val k,v; const char* H="header"; to_mdb_val(H, strlen(H), &k);
    if((rc = mdb_get(txn, d->dbi_meta, &k, &v))){ set_mdb_error(d,rc); mdb_txn_abort(txn); mdb_env_close(d->env); free(d); return NULL; }
    if(v.mv_size < sizeof(DbHeader)){ mdb_txn_abort(txn); mdb_env_close(d->env); free(d); return NULL; }
    memcpy(&d->header_cache, v.mv_data, sizeof(DbHeader));
    mdb_txn_abort(txn);
    d->load_state = INDEX_CORE_LOADED;
    d->bloom_file = INVALID_HANDLE_VALUE;
    d->bloom_buffer = NULL;
    d->bloom_buffer_len = 0;
    d->bloom_buffer_cap = 0;
    d->bloom_file_size = 0;
    d->bloom_offset = 0;
    *out_db = (Db*)d;
    return &d->header_cache;
}

void db_close(Db* db_){
    if(!db_) return;
    DbImpl* d = (DbImpl*)db_;
    if(d->wtxn){ mdb_txn_abort(d->wtxn); d->wtxn=NULL; }
    if(d->env){ mdb_env_close(d->env); }
    if(d->bloom_file && d->bloom_file!=INVALID_HANDLE_VALUE){ CloseHandle(d->bloom_file); }
    if(d->bloom_buffer){ free(d->bloom_buffer); d->bloom_buffer=NULL; d->bloom_buffer_cap=0; d->bloom_buffer_len=0; }
    free(d);
}

const DbHeader* db_header(Db* db_){
    DbImpl* d = (DbImpl*)db_;
    return &d->header_cache;
}
size_t db_current_mapsize(Db* db_){
    DbImpl* d = (DbImpl*)db_;
    MDB_envinfo info = {0};
    mdb_env_info(d->env, &info);
    return (size_t)info.me_mapsize;
}
size_t db_max_mapsize(Db* db_){
    DbImpl* d = (DbImpl*)db_;
    return d->map_max;
}
BOOL db_set_mapsize(Db* db_, size_t new_size_bytes){
    DbImpl* d = (DbImpl*)db_;
    if(new_size_bytes <= db_current_mapsize(db_) || new_size_bytes > d->map_max) return FALSE;
    int rc = mdb_env_set_mapsize(d->env, new_size_bytes);
    if(rc) set_mdb_error(d, rc); else set_error(d, DB_ERROR_NONE,0,NULL);
    if(rc==0){ d->header_cache.map_size_bytes = new_size_bytes; }
    return rc==0;
}
const DbError* db_last_error(Db* db_){
    DbImpl* d = (DbImpl*)db_;
    return &d->last_error;
}

size_t db_last_write_progress(Db* db_){
    DbImpl* d = (DbImpl*)db_;
    return d ? d->last_write_progress : 0;
}

BOOL db_ensure_loaded(Db* db_, IndexLoadState state){
    DbImpl* d = (DbImpl*)db_;
    int rc = ensure_indices(d, state);
    if(rc) set_mdb_error(d, rc); else set_error(d, DB_ERROR_NONE,0,NULL);
    return rc==0;
}

BOOL db_begin_write(Db* db_){
    DbImpl* d = (DbImpl*)db_;
    if(d->wtxn) return TRUE;
    int rc = ensure_indices(d, INDEX_CONTENT_LOADED);
    if(rc){ set_mdb_error(d, rc); return FALSE; }
    rc = mdb_txn_begin(d->env, NULL, 0, &d->wtxn);
    if(rc) set_mdb_error(d, rc); else set_error(d, DB_ERROR_NONE,0,NULL);
    return rc==0;
}
BOOL db_commit_write_ex(Db* db_, BOOL force_sync){
    DbImpl* d = (DbImpl*)db_;
    if(!d->wtxn) return TRUE;
    if(d->dirty){
        d->header_cache.generation++;
        d->header_cache.updated_time = now_filetime();
        MDB_val mk,mv; const char* H="header"; to_mdb_val(H, strlen(H), &mk);
        to_mdb_val(&d->header_cache, sizeof(d->header_cache), &mv);
        mdb_put(d->wtxn, d->dbi_meta, &mk, &mv, 0);
    }
    if(!bloom_buffer_flush(d)){
        mdb_txn_abort(d->wtxn);
        d->wtxn = NULL;
        return FALSE;
    }
    int rc = mdb_txn_commit(d->wtxn);
    if(rc) set_mdb_error(d, rc); else set_error(d, DB_ERROR_NONE,0,NULL);
    d->wtxn = NULL;
    if(rc==0){
        d->header_cache.map_size_bytes = db_current_mapsize(db_);
        mdb_env_sync(d->env, force_sync ? 1 : 0);
        d->dirty = FALSE;
    }
    return rc==0;
}
BOOL db_commit_write(Db* db_){
    return db_commit_write_ex(db_, TRUE);
}
void db_abort_write(Db* db_){
    DbImpl* d = (DbImpl*)db_;
    if(d->wtxn){ mdb_txn_abort(d->wtxn); d->wtxn=NULL; }
}

BOOL db_compress(Db* db_, const wchar_t* out_path){
    if(!db_ || !out_path) return FALSE;
    DbImpl* d = (DbImpl*)db_;
    if(d->wtxn){
        if(!db_commit_write(db_)) return FALSE;
    }
    char u8[MAX_PATH*3];
    int rc = WideCharToMultiByte(CP_UTF8,0,out_path,-1,u8,sizeof(u8),NULL,NULL);
    if(rc<=0){ set_sys_error(d, GetLastError()); return FALSE; }
    rc = mdb_env_copy2(d->env, u8, MDB_CP_COMPACT);
    if(rc) set_mdb_error(d, rc); else set_error(d, DB_ERROR_NONE,0,NULL);
    return rc==0;
}

// String interning helpers
static BOOL str_by_id_with_retry(DbImpl* d, uint64_t id, MDB_val* out, int max_retries, StringMeta* meta_out, BOOL* has_meta){
    MDB_val key; key.mv_data = &id; key.mv_size = sizeof(id);
    if(d->wtxn){
        if(mdb_get(d->wtxn, d->dbi_strings, &key, out) == 0){
            string_value_parse(out, out, meta_out, has_meta);
            return TRUE;
        }
        return FALSE;
    }
    for(int i = 0; i < max_retries; ++i){
        MDB_txn* txn;
        if(mdb_txn_begin(d->env, NULL, MDB_RDONLY, &txn) == 0){
            int rc = mdb_get(txn, d->dbi_strings, &key, out);
            if(rc == 0){
                string_value_parse(out, out, meta_out, has_meta);
                mdb_txn_abort(txn);
                return TRUE;
            }
            mdb_txn_abort(txn);
        }
        Sleep(1 << i);
    }
    return FALSE;
}

typedef struct InternTask {
    size_t index;
    const wchar_t* wide;
    char* utf8;
    size_t utf8_len;
    uint64_t utf8_hash;
    uint64_t id;
    struct InternTask* leader;
    BOOL need_normalized;
    uint64_t normalized_id;
} InternTask;

typedef struct NormalizedTask {
    InternTask* owner;
    char* utf8;
    size_t utf8_len;
    uint64_t utf8_hash;
    uint64_t id;
    struct NormalizedTask* leader;
} NormalizedTask;

static int intern_task_cmp(const void* a, const void* b){
    const InternTask* const* pa = (const InternTask* const*)a;
    const InternTask* const* pb = (const InternTask* const*)b;
    if((*pa)->utf8_hash < (*pb)->utf8_hash) return -1;
    if((*pa)->utf8_hash > (*pb)->utf8_hash) return 1;
    if((*pa)->utf8_len < (*pb)->utf8_len) return -1;
    if((*pa)->utf8_len > (*pb)->utf8_len) return 1;
    return strcmp((*pa)->utf8, (*pb)->utf8);
}

static int normalized_task_cmp(const void* a, const void* b){
    const NormalizedTask* const* pa = (const NormalizedTask* const*)a;
    const NormalizedTask* const* pb = (const NormalizedTask* const*)b;
    if((*pa)->utf8_hash < (*pb)->utf8_hash) return -1;
    if((*pa)->utf8_hash > (*pb)->utf8_hash) return 1;
    if((*pa)->utf8_len < (*pb)->utf8_len) return -1;
    if((*pa)->utf8_len > (*pb)->utf8_len) return 1;
    return strcmp((*pa)->utf8, (*pb)->utf8);
}


static void db_intern_wstrings_batched(Db* db_, const wchar_t* const* strings, const uint8_t* need_normalized_flags, size_t count, uint64_t* out_ids, uint64_t* out_normalized_ids){
    if(!db_ || !out_ids || !strings || count == 0){
        if(out_normalized_ids){
            for(size_t i = 0; i < count; ++i){
                out_normalized_ids[i] = 0;
            }
        }
        return;
    }
    DbImpl* d = (DbImpl*)db_;
    size_t task_cap = count;
    InternTask stack_tasks[8];
    InternTask* tasks = (count <= ARRAYSIZE(stack_tasks)) ? stack_tasks : (InternTask*)malloc(sizeof(InternTask) * task_cap);
    InternTask** order = NULL;
    InternTask** unique = NULL;
    NormalizedTask* norm_tasks = NULL;
    NormalizedTask** norm_order = NULL;
    NormalizedTask** norm_unique = NULL;
    size_t task_count = 0;
    size_t valid_tasks = 0;
    size_t unique_count = 0;
    size_t norm_valid = 0;
    size_t norm_unique_count = 0;
    BOOL any_need_normalized = FALSE;
    if(!tasks){
        if(out_normalized_ids){
            for(size_t i = 0; i < count; ++i){
                out_normalized_ids[i] = 0;
            }
        }
        return;
    }
    for(size_t i = 0; i < count; ++i){
        if(out_normalized_ids){
            out_normalized_ids[i] = 0;
        }
        const wchar_t* ws = strings[i];
        if(!ws || !ws[0]){
            out_ids[i] = 0;
            continue;
        }
        InternTask task;
        memset(&task, 0, sizeof(task));
        task.index = i;
        task.wide = ws;
        task.need_normalized = (need_normalized_flags && need_normalized_flags[i]) ? TRUE : FALSE;
        if(task.need_normalized) any_need_normalized = TRUE;
        if(task_count < task_cap){
            tasks[task_count++] = task;
        }
    }
    if(task_count == 0) goto cleanup;
    order = (InternTask**)malloc(sizeof(InternTask*) * task_count);
    unique = (InternTask**)malloc(sizeof(InternTask*) * task_count);
    if(!order || !unique) goto cleanup;
    for(size_t i = 0; i < task_count; ++i){
        InternTask* t = &tasks[i];
        int needed = WideCharToMultiByte(CP_UTF8, 0, t->wide, -1, NULL, 0, NULL, NULL);
        if(needed <= 0){
            out_ids[t->index] = 0;
            continue;
        }
        char* buf = (char*)malloc((size_t)needed);
        if(!buf){
            out_ids[t->index] = 0;
            continue;
        }
        int rc = WideCharToMultiByte(CP_UTF8, 0, t->wide, -1, buf, needed, NULL, NULL);
        if(rc <= 0){
            free(buf);
            out_ids[t->index] = 0;
            continue;
        }
        t->utf8 = buf;
        t->utf8_len = (size_t)(needed - 1);
        t->utf8_hash = hash64(buf, t->utf8_len);
        order[valid_tasks++] = t;
    }
    if(valid_tasks == 0) goto cleanup;
    qsort(order, valid_tasks, sizeof(InternTask*), intern_task_cmp);
    InternTask* prev = NULL;
    for(size_t i = 0; i < valid_tasks; ++i){
        InternTask* t = order[i];
        if(prev && prev->utf8_hash == t->utf8_hash && prev->utf8_len == t->utf8_len && strcmp(prev->utf8, t->utf8) == 0){
            t->leader = prev->leader ? prev->leader : prev;
        } else {
            t->leader = t;
            unique[unique_count++] = t;
            prev = t;
        }
        prev = t;
    }
    for(size_t i = 0; i < valid_tasks; ++i){
        InternTask* t = order[i];
        InternTask* leader = t->leader ? t->leader : t;
        if(t->need_normalized && leader){
            leader->need_normalized = TRUE;
        }
    }
    for(size_t i = 0; i < unique_count; ++i){
        InternTask* t = unique[i];
        t->id = string_cache_lookup(t->utf8, t->utf8_hash);
    }
    MDB_txn* rtxn = d->wtxn ? d->wtxn : NULL;
    BOOL need_abort = FALSE;
    if(!rtxn){
        int rc = mdb_txn_begin(d->env, NULL, MDB_RDONLY, &rtxn);
        if(rc == 0){
            need_abort = TRUE;
        } else {
            rtxn = NULL;
            set_mdb_error(d, rc);
        }
    }
    if(rtxn){
        for(size_t i = 0; i < unique_count; ++i){
            InternTask* t = unique[i];
            if(t->id) continue;
            MDB_val k = {.mv_data = t->utf8, .mv_size = t->utf8_len};
            MDB_val v;
            if(mdb_get(rtxn, d->dbi_strrev, &k, &v) == 0){
                t->id = *(uint64_t*)v.mv_data;
                string_cache_insert(t->utf8, t->utf8_hash, t->id);
            }
        }
    }
    uint64_t original_count = d->header_cache.string_count;
    BOOL header_dirty = FALSE;
    for(size_t i = 0; i < unique_count; ++i){
        InternTask* t = unique[i];
        if(t->id) continue;
        if(!d->wtxn && !db_begin_write(db_)){
            d->header_cache.string_count = original_count;
            goto cleanup;
        }
        uint64_t new_id = d->header_cache.string_count + 1;
        d->header_cache.string_count = new_id;
        MDB_val idkey = {.mv_data = &new_id, .mv_size = sizeof(new_id)};
        size_t total_len = t->utf8_len + sizeof(StringMeta);
        uint8_t* storage = (uint8_t*)malloc(total_len);
        if(!storage){
            d->header_cache.string_count = original_count;
            set_error(d, DB_ERROR_OS, 0, "out of memory");
            goto cleanup;
        }
        memcpy(storage, t->utf8, t->utf8_len);
        StringMeta placeholder; string_meta_init(&placeholder);
        memcpy(storage + t->utf8_len, &placeholder, sizeof(placeholder));
        MDB_val idval = {.mv_data = storage, .mv_size = total_len};
        int rc = mdb_put(d->wtxn, d->dbi_strings, &idkey, &idval, 0);
        free(storage);
        if(rc){
            d->header_cache.string_count = original_count;
            set_mdb_error(d, rc);
            goto cleanup;
        }
        MDB_val revkey = {.mv_data = t->utf8, .mv_size = t->utf8_len};
        MDB_val revid = {.mv_data = &new_id, .mv_size = sizeof(new_id)};
        rc = mdb_put(d->wtxn, d->dbi_strrev, &revkey, &revid, 0);
        if(rc){
            d->header_cache.string_count = original_count;
            set_mdb_error(d, rc);
            goto cleanup;
        }
        t->id = new_id;
        string_cache_insert(t->utf8, t->utf8_hash, t->id);
        header_dirty = TRUE;
    }
    if(any_need_normalized && unique_count){
        norm_tasks = (NormalizedTask*)malloc(sizeof(NormalizedTask) * unique_count);
        norm_order = (NormalizedTask**)malloc(sizeof(NormalizedTask*) * unique_count);
        norm_unique = (NormalizedTask**)malloc(sizeof(NormalizedTask*) * unique_count);
        if(norm_tasks && norm_order && norm_unique){
            for(size_t i = 0; i < unique_count; ++i){
                InternTask* leader = unique[i];
                if(!leader->need_normalized) continue;
                NormalizedTask nt; memset(&nt, 0, sizeof(nt));
                size_t bufcap = leader->utf8_len + 1;
                if(bufcap == 0) bufcap = 1;
                char* buf = (char*)malloc(bufcap);
                if(!buf) continue;
                normalize_filename_utf8(leader->utf8, buf, bufcap);
                size_t nlen = strlen(buf);
                nt.owner = leader;
                nt.utf8 = buf;
                nt.utf8_len = nlen;
                nt.utf8_hash = hash64(buf, nlen);
                norm_tasks[norm_valid] = nt;
                norm_order[norm_valid] = &norm_tasks[norm_valid];
                norm_valid++;
            }
            if(norm_valid){
                qsort(norm_order, norm_valid, sizeof(NormalizedTask*), normalized_task_cmp);
                NormalizedTask* prev_norm = NULL;
                for(size_t i = 0; i < norm_valid; ++i){
                    NormalizedTask* nt = norm_order[i];
                    if(prev_norm && prev_norm->utf8_hash == nt->utf8_hash && prev_norm->utf8_len == nt->utf8_len && strcmp(prev_norm->utf8, nt->utf8) == 0){
                        nt->leader = prev_norm->leader ? prev_norm->leader : prev_norm;
                    } else {
                        nt->leader = nt;
                        norm_unique[norm_unique_count++] = nt;
                        prev_norm = nt;
                    }
                    prev_norm = nt;
                }
                for(size_t i = 0; i < norm_unique_count; ++i){
                    NormalizedTask* nt = norm_unique[i];
                    nt->id = string_cache_lookup(nt->utf8, nt->utf8_hash);
                }
                if(rtxn){
                    for(size_t i = 0; i < norm_unique_count; ++i){
                        NormalizedTask* nt = norm_unique[i];
                        if(nt->id) continue;
                        MDB_val k = {.mv_data = nt->utf8, .mv_size = nt->utf8_len};
                        MDB_val v;
                        if(mdb_get(rtxn, d->dbi_strrev, &k, &v) == 0){
                            nt->id = *(uint64_t*)v.mv_data;
                            string_cache_insert(nt->utf8, nt->utf8_hash, nt->id);
                        }
                    }
                }
                for(size_t i = 0; i < norm_unique_count; ++i){
                    NormalizedTask* nt = norm_unique[i];
                    if(nt->id) continue;
                    if(!d->wtxn && !db_begin_write(db_)){
                        d->header_cache.string_count = original_count;
                        goto cleanup;
                    }
                    uint64_t new_id = d->header_cache.string_count + 1;
                    d->header_cache.string_count = new_id;
                    MDB_val idkey = {.mv_data = &new_id, .mv_size = sizeof(new_id)};
                    size_t total_len = nt->utf8_len + sizeof(StringMeta);
                    uint8_t* storage = (uint8_t*)malloc(total_len);
                    if(!storage){
                        d->header_cache.string_count = original_count;
                        set_error(d, DB_ERROR_OS, 0, "out of memory");
                        goto cleanup;
                    }
                    memcpy(storage, nt->utf8, nt->utf8_len);
                    StringMeta placeholder; string_meta_init(&placeholder);
                    memcpy(storage + nt->utf8_len, &placeholder, sizeof(placeholder));
                    MDB_val idval = {.mv_data = storage, .mv_size = total_len};
                    int rc = mdb_put(d->wtxn, d->dbi_strings, &idkey, &idval, 0);
                    free(storage);
                    if(rc){
                        d->header_cache.string_count = original_count;
                        set_mdb_error(d, rc);
                        goto cleanup;
                    }
                    MDB_val revkey = {.mv_data = nt->utf8, .mv_size = nt->utf8_len};
                    MDB_val revid = {.mv_data = &new_id, .mv_size = sizeof(new_id)};
                    rc = mdb_put(d->wtxn, d->dbi_strrev, &revkey, &revid, 0);
                    if(rc){
                        d->header_cache.string_count = original_count;
                        set_mdb_error(d, rc);
                        goto cleanup;
                    }
                    nt->id = new_id;
                    string_cache_insert(nt->utf8, nt->utf8_hash, nt->id);
                    header_dirty = TRUE;
                }
                for(size_t i = 0; i < norm_valid; ++i){
                    NormalizedTask* nt = norm_order[i];
                    NormalizedTask* leader = nt->leader ? nt->leader : nt;
                    if(leader->id){
                        nt->owner->normalized_id = leader->id;
                    }
                }
            }
        }
    }
    if(header_dirty){
        MDB_val mk, mv; const char* H = "header";
        to_mdb_val(H, strlen(H), &mk);
        to_mdb_val(&d->header_cache, sizeof(d->header_cache), &mv);
        int rc = mdb_put(d->wtxn, d->dbi_meta, &mk, &mv, 0);
        if(rc){
            d->header_cache.string_count = original_count;
            set_mdb_error(d, rc);
            goto cleanup;
        }
    }
    for(size_t i = 0; i < valid_tasks; ++i){
        InternTask* t = order[i];
        InternTask* leader = t->leader ? t->leader : t;
        out_ids[t->index] = leader->id;
        if(out_normalized_ids){
            if(leader->need_normalized){
                out_normalized_ids[t->index] = leader->normalized_id;
            }
        }
    }
cleanup:
    if(need_abort && rtxn){
        mdb_txn_abort(rtxn);
    }
    if(order){
        for(size_t i = 0; i < valid_tasks; ++i){
            InternTask* t = order[i];
            if(t->utf8){
                free(t->utf8);
                t->utf8 = NULL;
            }
        }
        free(order);
    }
    if(unique) free(unique);
    if(norm_order) free(norm_order);
    if(norm_unique) free(norm_unique);
    if(norm_tasks){
        for(size_t i = 0; i < norm_valid; ++i){
            if(norm_tasks[i].utf8){
                free(norm_tasks[i].utf8);
                norm_tasks[i].utf8 = NULL;
            }
        }
        free(norm_tasks);
    }
    if(tasks && tasks != stack_tasks) free(tasks);
}


uint64_t db_intern_wstring(Db* db_, const wchar_t* s){
    if(!s || !s[0]) return 0;
    uint64_t result = 0;
    const wchar_t* array[1] = { s };
    uint64_t ids[1] = { 0 };
    db_intern_wstrings_batched(db_, array, NULL, 1, ids, NULL);
    result = ids[0];
    return result;
}

// ---- Trigram helpers ----

void extract_trigrams(const char* text, uint32_t** out_tris, size_t* out_count){
    // Limit the amount of data we look at to avoid reading past the end of a
    // malformed or extremely long string.  This mirrors the bloom filter limit
    // used elsewhere in the codebase.
    size_t len = strnlen(text, DB_BLOOM_MAX_BYTES);
    if(len < 3 || len == DB_BLOOM_MAX_BYTES){
        *out_tris = NULL;
        *out_count = 0;
        return;
    }

    size_t need = len - 2;
    static uint32_t* pool = NULL;
    static size_t pool_cap = 0;
    if(pool_cap < need){
        if(need > SIZE_MAX / sizeof(uint32_t)){
            *out_tris = NULL;
            *out_count = 0;
            return;
        }
        uint32_t* nb = (uint32_t*)realloc(pool, need * sizeof(uint32_t));
        if(!nb){ *out_tris = NULL; *out_count = 0; return; }
        pool = nb; pool_cap = need;
    }

#if defined(__SSE4_1__)
    size_t i = 0;
    if(len >= 16){
        size_t simd_end = len - 16;
        for(; i <= simd_end; i += 4){
            __m128i bytes = _mm_loadu_si128((const __m128i*)(text + i));
            __m128i b0 = _mm_cvtepu8_epi32(bytes);
            __m128i b1 = _mm_cvtepu8_epi32(_mm_srli_si128(bytes,1));
            __m128i b2 = _mm_cvtepu8_epi32(_mm_srli_si128(bytes,2));
            __m128i t  = _mm_or_si128(_mm_slli_epi32(b0,16),
                              _mm_or_si128(_mm_slli_epi32(b1,8), b2));
            _mm_storeu_si128((__m128i*)(pool + i), t);
        }
    }
    for(; i < need; ++i){
        pool[i] = ((uint8_t)text[i]<<16)|((uint8_t)text[i+1]<<8)|((uint8_t)text[i+2]);
    }
#else
    for(size_t i=0;i<need;i++){
        pool[i]=((uint8_t)text[i]<<16)|((uint8_t)text[i+1]<<8)|((uint8_t)text[i+2]);
    }
#endif
    *out_tris = pool;
    *out_count = need;
}

static int compare_u32(const void* a, const void* b){
    uint32_t ua = *(const uint32_t*)a;
    uint32_t ub = *(const uint32_t*)b;
    if(ua < ub) return -1;
    if(ua > ub) return 1;
    return 0;
}

static void emit_trigrams(DbImpl* d, const char* name_u8, uint64_t name_id, NameBloomContext* prepared){
    if(!d || !name_u8 || !name_u8[0]) return;
    NameBloomContext local;
    NameBloomContext* ctx = prepared;
    BOOL need_release = FALSE;
    if(!ctx){
        if(!name_bloom_context_prepare(name_u8, &local)) return;
        ctx = &local;
        need_release = TRUE;
    }
    if(ctx->trigram_count == 0){
        if(need_release) name_bloom_context_release(ctx);
        return;
    }

    qsort(ctx->trigrams, ctx->trigram_count, sizeof(uint32_t), compare_u32);

    for(size_t i=0;i<ctx->trigram_count;i++){
        uint32_t key = ctx->trigrams[i];
        if(i > 0 && key == ctx->trigrams[i-1]) continue;

        MDB_val k={.mv_data=&key,.mv_size=3}, v={.mv_data=&name_id,.mv_size=sizeof(name_id)};
        int rc = mdb_put(d->wtxn, d->dbi_trigram_index, &k, &v, MDB_NODUPDATA);
        if(rc && rc!=MDB_KEYEXIST){ set_mdb_error(d, rc); }
    }
    if(need_release) name_bloom_context_release(ctx);
}

static void remove_trigrams(DbImpl* d, const char* name_u8, uint64_t name_id){
    size_t len = strlen(name_u8);
    if(len < 3) return;
    char stack_tmp[512];
    BOOL heap = len + 1 > sizeof(stack_tmp);
    char* tmp = heap ? (char*)malloc(len + 1) : stack_tmp;
    if(!tmp) return;
    memcpy(tmp, name_u8, len + 1);
    BOOL needs_lower = FALSE;
    for(size_t i = 0; i < len; ++i){
        unsigned char c = (unsigned char)tmp[i];
        if(c >= 'A' && c <= 'Z'){
            needs_lower = TRUE;
            break;
        }
    }
    if(needs_lower){
        lowercase_ascii(tmp, len);
    }

    uint32_t* tris; size_t tri_n;
    extract_trigrams(tmp, &tris, &tri_n);
    if(!tris || tri_n == 0){
        if(heap) free(tmp);
        return;
    }

    qsort(tris, tri_n, sizeof(uint32_t), compare_u32);

    for(size_t i=0;i<tri_n;i++){
        uint32_t key = tris[i];
        if(i > 0 && key == tris[i-1]) continue;
        MDB_val k={.mv_data=&key,.mv_size=3}, v={.mv_data=&name_id,.mv_size=sizeof(name_id)};
        int rc = mdb_del(d->wtxn, d->dbi_trigram_index, &k, &v);
        if(rc && rc!=MDB_NOTFOUND){ set_mdb_error(d, rc); }
    }
    if(heap) free(tmp);
}

BOOL db_get_compressed_trigram(Db* db_, uint32_t trigram, CompressedTrigram* out){
    if(!db_ || !out) return FALSE;
    DbImpl* d = (DbImpl*)db_;
    if(!db_ensure_loaded(db_, INDEX_CONTENT_LOADED)) return FALSE;
    MDB_txn* txn; if(mdb_txn_begin(d->env, NULL, MDB_RDONLY, &txn)!=0) return FALSE;
    MDB_cursor* c=NULL; if(mdb_cursor_open(txn, d->dbi_trigram_index, &c)!=0){ mdb_txn_abort(txn); return FALSE; }
    uint8_t key_bytes[3]={ (uint8_t)(trigram>>16), (uint8_t)(trigram>>8), (uint8_t)trigram };
    MDB_val k={.mv_data=key_bytes,.mv_size=3}, v;
    uint32_t cap=0, n=0; uint32_t* buf=NULL; uint64_t prev=0;
    if(mdb_cursor_get(c,&k,&v,MDB_SET_KEY)==0){
        do{
            if(n==cap){
                uint32_t newcap = cap?cap*2:16;
                uint32_t* nb = (uint32_t*)realloc(buf, newcap*sizeof(uint32_t));
                if(!nb){ free(buf); mdb_cursor_close(c); mdb_txn_abort(txn); return FALSE; }
                buf = nb; cap = newcap;
            }
            uint64_t id = *(uint64_t*)v.mv_data;
            buf[n++] = (uint32_t)(id - prev);
            prev = id;
        }while(mdb_cursor_get(c,&k,&v,MDB_NEXT_DUP)==0);
    }
    mdb_cursor_close(c); mdb_txn_abort(txn);
    out->trigram.bytes[0]=key_bytes[0];
    out->trigram.bytes[1]=key_bytes[1];
    out->trigram.bytes[2]=key_bytes[2];
    out->string_id_count=n;
    out->string_ids=buf;
    return TRUE;
}

void db_free_compressed_trigram(CompressedTrigram* ct){
    if(ct && ct->string_ids){ free(ct->string_ids); ct->string_ids=NULL; ct->string_id_count=0; }
}



BOOL db_get_index_state(Db* db_, IndexState* out){
    DbImpl* d = (DbImpl*)db_;
    MDB_txn* txn; if(mdb_txn_begin(d->env,NULL,MDB_RDONLY,&txn)!=0) return FALSE;
    MDB_val k={.mv_data="index_state",.mv_size=11}, v;
    int rc = mdb_get(txn, d->dbi_meta, &k, &v);
    if(rc==0 && v.mv_size==sizeof(IndexState)){ memcpy(out, v.mv_data, sizeof(IndexState)); mdb_txn_abort(txn); return TRUE; }
    mdb_txn_abort(txn); return FALSE;
}
BOOL db_set_index_state(Db* db_, const IndexState* st){
    DbImpl* d = (DbImpl*)db_;
    if(!d->wtxn){ if(!db_begin_write(db_)) return FALSE; }
    MDB_val k={.mv_data="index_state",.mv_size=11}, v={.mv_data=(void*)st,.mv_size=sizeof(IndexState)};
    int rc = mdb_put(d->wtxn, d->dbi_meta, &k, &v, 0);
    if(rc) set_mdb_error(d, rc); else set_error(d, DB_ERROR_NONE,0,NULL);
    return rc==0;
}

// ---- Record write ----
static BOOL record_is_names_only(const DbRecord* r){
    if(!r) return FALSE;
    if(r->file_size != 0) return FALSE;
    if(r->creation_time != 0 || r->modified_time != 0 || r->access_time != 0) return FALSE;
    if(r->content_str_id || r->author_str_id || r->camera_str_id || r->lens_str_id) return FALSE;
    if(r->artist_str_id || r->album_str_id || r->title_str_id || r->preview_str_id) return FALSE;
    if(r->hash_crc != 0) return FALSE;
    return TRUE;
}

BOOL db_put_records(Db* db_, const DbRecord* recs, size_t count){
    DbImpl* d = (DbImpl*)db_;
    if(!d->wtxn){ if(!db_begin_write(db_)) return FALSE; }
    if(count == 0){ set_error(d, DB_ERROR_NONE, 0, NULL); return TRUE; }

    const size_t MAX_BATCH_RETRIES = 3;
    size_t attempt = 0;
    DbHeader header_before = d->header_cache;
    uint64_t bloom_offset_before = d->bloom_offset;
    uint64_t bloom_file_size_before = d->bloom_file_size;
    size_t bloom_buffer_len_before = d->bloom_buffer_len;
    BOOL dirty_before = d->dirty;
    MDB_txn* parent_txn = d->wtxn;
    d->last_write_progress = 0;
    BOOL result = TRUE;

    IndexEntry64* fname_entries = NULL; size_t fname_count = 0, fname_cap = 0;
    IndexEntry64* parent_entries = NULL; size_t parent_count = 0, parent_cap = 0;
    IndexEntry64* path_entries = NULL; size_t path_count = 0, path_cap = 0;
    IndexEntry64* size_entries = NULL; size_t size_count = 0, size_cap = 0;
    IndexEntry64* date_entries = NULL; size_t date_count = 0, date_cap = 0;
    IndexEntry64* mtime_entries = NULL; size_t mtime_count = 0, mtime_cap = 0;
    IndexEntry32* attr_entries = NULL; size_t attr_count = 0, attr_cap = 0;
    ExtensionEntry* ext_entries = NULL; size_t ext_count = 0, ext_cap = 0;
    IndexEntry64* content_entries = NULL; size_t content_count = 0, content_cap = 0;
    IndexEntry64* author_entries = NULL; size_t author_count = 0, author_cap = 0;
    IndexEntry64* camera_entries = NULL; size_t camera_count = 0, camera_cap = 0;
    IndexEntry64* lens_entries = NULL; size_t lens_count = 0, lens_cap = 0;
    IndexEntry64* artist_entries = NULL; size_t artist_count = 0, artist_cap = 0;
    IndexEntry64* album_entries = NULL; size_t album_count = 0, album_cap = 0;
    IndexEntry64* title_entries = NULL; size_t title_count = 0, title_cap = 0;

retry_batch:
    if(attempt++ >= MAX_BATCH_RETRIES){
        d->dirty = dirty_before;
        d->header_cache = header_before;
        d->bloom_offset = bloom_offset_before;
        LARGE_INTEGER li; li.QuadPart = (LONGLONG)bloom_offset_before;
        SetFilePointerEx(d->bloom_file, li, NULL, FILE_BEGIN);
        SetEndOfFile(d->bloom_file);
        d->bloom_file_size = bloom_file_size_before;
        d->bloom_buffer_len = bloom_buffer_len_before;
        result = FALSE;
        goto cleanup;
    }

    MDB_txn* batch_txn = NULL;
    int rc = mdb_txn_begin(d->env, parent_txn, 0, &batch_txn);
    if(rc){
        d->dirty = dirty_before;
        set_mdb_error(d, rc);
        d->header_cache = header_before;
        d->bloom_offset = bloom_offset_before;
        result = FALSE;
        goto cleanup;
    }

    DbHeader header_tmp = header_before;
    uint64_t bloom_offset_tmp = bloom_offset_before;
    size_t processed = 0;
    BOOL success = TRUE;

    d->wtxn = batch_txn;

    LARGE_INTEGER bloom_seek;
    bloom_seek.QuadPart = (LONGLONG)bloom_offset_tmp;
    if(!SetFilePointerEx(d->bloom_file, bloom_seek, NULL, FILE_BEGIN)){
        set_sys_error(d, GetLastError());
        success = FALSE;
    }

    fname_count = parent_count = path_count = size_count = date_count = mtime_count = 0;
    attr_count = 0;
    ext_count = 0;
    content_count = author_count = camera_count = lens_count = artist_count = album_count = title_count = 0;

    for(; success && processed<count; ++processed){
        const DbRecord* r = &recs[processed];
        BOOL names_only = record_is_names_only(r);
        header_tmp.record_count++;
        uint64_t id = header_tmp.record_count;
        MDB_val k,v; to_mdb_val(&id, sizeof(id), &k); to_mdb_val((void*)r, sizeof(*r), &v);
        if((rc = mdb_put(d->wtxn, d->dbi_records, &k, &v, 0))){ success = FALSE; set_mdb_error(d,rc); break; }
        uint64_t fname_key = r->normalized_name_str_id ? r->normalized_name_str_id : r->name_str_id;
        if(!append_index64(&fname_entries, &fname_count, &fname_cap, fname_key, id) ||
           !append_index64(&parent_entries, &parent_count, &parent_cap, r->parent_str_id, id) ||
           !append_index64(&path_entries, &path_count, &path_cap, r->parent_str_id, id)){
            set_error(d, DB_ERROR_OS, 0, "out of memory");
            success = FALSE;
            break;
        }
        if(names_only){
            continue;
        }
        // size_index (files only)
        if(r->type == DB_REC_FILE){
            if(!append_index64(&size_entries, &size_count, &size_cap, r->file_size, id)){
                set_error(d, DB_ERROR_OS, 0, "out of memory");
                success = FALSE;
                break;
            }
        }
        // date_index (modified time day)
        uint64_t day = filetime_days(r->modified_time);
        if(!append_index64(&date_entries, &date_count, &date_cap, day, id) ||
           !append_index64(&mtime_entries, &mtime_count, &mtime_cap, r->modified_time, id) ||
           !append_index32(&attr_entries, &attr_count, &attr_cap, r->attributes, id)){
            set_error(d, DB_ERROR_OS, 0, "out of memory");
            success = FALSE;
            break;
        }
        // extension_index & trigrams from name
        // Fetch UTF-8 name by id
        MDB_val namev;
        StringMeta name_meta; BOOL name_has_meta = FALSE;
        MDB_val normv;
        StringMeta norm_meta; BOOL norm_has_meta = FALSE;
        BOOL have_name = success && str_by_id_with_retry(d, r->name_str_id, &namev, 5, &name_meta, &name_has_meta);
        BOOL have_norm = success && r->normalized_name_str_id &&
                         str_by_id_with_retry(d, r->normalized_name_str_id, &normv, 5, &norm_meta, &norm_has_meta);
        if(success && have_norm){
            NameBloomContext norm_ctx;
            if(!name_bloom_context_prepare((const char*)normv.mv_data, &norm_ctx)){
                success = FALSE;
                set_error(d, DB_ERROR_OS, 0, "failed to prepare bloom context");
                break;
            }
            if(!ensure_bloom_for_string(d, r->normalized_name_str_id, &normv, &norm_ctx, &norm_meta, &bloom_offset_tmp)){
                success = FALSE;
                name_bloom_context_release(&norm_ctx);
                break;
            }
            emit_trigrams(d, (const char*)normv.mv_data, r->normalized_name_str_id, &norm_ctx);
            name_bloom_context_release(&norm_ctx);
        } else if(success && have_name){
            NameBloomContext name_ctx;
            if(!name_bloom_context_prepare((const char*)namev.mv_data, &name_ctx)){
                success = FALSE;
                set_error(d, DB_ERROR_OS, 0, "failed to prepare bloom context");
                break;
            }
            if(!ensure_bloom_for_string(d, r->name_str_id, &namev, &name_ctx, &name_meta, &bloom_offset_tmp)){
                success = FALSE;
                name_bloom_context_release(&name_ctx);
                break;
            }
            emit_trigrams(d, (const char*)namev.mv_data, r->name_str_id, &name_ctx);
            name_bloom_context_release(&name_ctx);
        }
        if(success && have_name){
            char ext[32]; split_extension_utf8((const char*)namev.mv_data, ext, sizeof(ext));
            if(ext[0]){
                size_t ext_len = strlen(ext);
                if(!append_extension_entry(&ext_entries, &ext_count, &ext_cap, ext, ext_len, id)){
                    set_error(d, DB_ERROR_OS, 0, "out of memory");
                    success = FALSE;
                    break;
                }
            }
        }
        if(r->content_str_id){
            if(!append_index64(&content_entries, &content_count, &content_cap, r->content_str_id, id)){
                set_error(d, DB_ERROR_OS, 0, "out of memory");
                success = FALSE;
                break;
            }
            MDB_val cvstr;
            StringMeta content_meta; BOOL content_has_meta = FALSE;
            if(success && str_by_id_with_retry(d, r->content_str_id, &cvstr, 5, &content_meta, &content_has_meta)){
                NameBloomContext content_ctx;
                if(!name_bloom_context_prepare((const char*)cvstr.mv_data, &content_ctx)){
                    success = FALSE;
                    set_error(d, DB_ERROR_OS, 0, "failed to prepare bloom context");
                    break;
                }
                if(!ensure_bloom_for_string(d, r->content_str_id, &cvstr, &content_ctx, &content_meta, &bloom_offset_tmp)){
                    success = FALSE;
                    name_bloom_context_release(&content_ctx);
                    break;
                }
                emit_trigrams(d, (const char*)cvstr.mv_data, r->content_str_id, &content_ctx);
                name_bloom_context_release(&content_ctx);
            }
        }
        if(r->author_str_id){
            if(!append_index64(&author_entries, &author_count, &author_cap, r->author_str_id, id)){
                set_error(d, DB_ERROR_OS, 0, "out of memory");
                success = FALSE;
                break;
            }
        }
        if(r->camera_str_id){
            if(!append_index64(&camera_entries, &camera_count, &camera_cap, r->camera_str_id, id)){
                set_error(d, DB_ERROR_OS, 0, "out of memory");
                success = FALSE;
                break;
            }
        }
        if(r->lens_str_id){
            if(!append_index64(&lens_entries, &lens_count, &lens_cap, r->lens_str_id, id)){
                set_error(d, DB_ERROR_OS, 0, "out of memory");
                success = FALSE;
                break;
            }
        }
        if(r->artist_str_id){
            if(!append_index64(&artist_entries, &artist_count, &artist_cap, r->artist_str_id, id)){
                set_error(d, DB_ERROR_OS, 0, "out of memory");
                success = FALSE;
                break;
            }
        }
        if(r->album_str_id){
            if(!append_index64(&album_entries, &album_count, &album_cap, r->album_str_id, id)){
                set_error(d, DB_ERROR_OS, 0, "out of memory");
                success = FALSE;
                break;
            }
        }
        if(r->title_str_id){
            if(!append_index64(&title_entries, &title_count, &title_cap, r->title_str_id, id)){
                set_error(d, DB_ERROR_OS, 0, "out of memory");
                success = FALSE;
                break;
            }
        }
    }
    if(success){
        for(size_t i=0; success && i<fname_count; ++i){
            MDB_val key = {.mv_data=&fname_entries[i].key, .mv_size=sizeof(uint64_t)};
            MDB_val val = {.mv_data=&fname_entries[i].value, .mv_size=sizeof(uint64_t)};
            rc = mdb_put(d->wtxn, d->dbi_fname_index, &key, &val, MDB_NODUPDATA);
            if(rc && rc!=MDB_KEYEXIST){ success = FALSE; set_mdb_error(d,rc); }
        }
        for(size_t i=0; success && i<parent_count; ++i){
            MDB_val key = {.mv_data=&parent_entries[i].key, .mv_size=sizeof(uint64_t)};
            MDB_val val = {.mv_data=&parent_entries[i].value, .mv_size=sizeof(uint64_t)};
            rc = mdb_put(d->wtxn, d->dbi_parent_index, &key, &val, MDB_NODUPDATA);
            if(rc && rc!=MDB_KEYEXIST){ success = FALSE; set_mdb_error(d,rc); }
        }
        for(size_t i=0; success && i<path_count; ++i){
            MDB_val key = {.mv_data=&path_entries[i].key, .mv_size=sizeof(uint64_t)};
            MDB_val val = {.mv_data=&path_entries[i].value, .mv_size=sizeof(uint64_t)};
            rc = mdb_put(d->wtxn, d->dbi_path_hierarchy, &key, &val, MDB_NODUPDATA);
            if(rc && rc!=MDB_KEYEXIST){ success = FALSE; set_mdb_error(d,rc); }
        }
        for(size_t i=0; success && i<size_count; ++i){
            MDB_val key = {.mv_data=&size_entries[i].key, .mv_size=sizeof(uint64_t)};
            MDB_val val = {.mv_data=&size_entries[i].value, .mv_size=sizeof(uint64_t)};
            rc = mdb_put(d->wtxn, d->dbi_size_index, &key, &val, MDB_NODUPDATA);
            if(rc && rc!=MDB_KEYEXIST){ success = FALSE; set_mdb_error(d,rc); }
        }
        for(size_t i=0; success && i<date_count; ++i){
            MDB_val key = {.mv_data=&date_entries[i].key, .mv_size=sizeof(uint64_t)};
            MDB_val val = {.mv_data=&date_entries[i].value, .mv_size=sizeof(uint64_t)};
            rc = mdb_put(d->wtxn, d->dbi_date_index, &key, &val, MDB_NODUPDATA);
            if(rc && rc!=MDB_KEYEXIST){ success = FALSE; set_mdb_error(d,rc); }
        }
        for(size_t i=0; success && i<mtime_count; ++i){
            MDB_val key = {.mv_data=&mtime_entries[i].key, .mv_size=sizeof(uint64_t)};
            MDB_val val = {.mv_data=&mtime_entries[i].value, .mv_size=sizeof(uint64_t)};
            rc = mdb_put(d->wtxn, d->dbi_mtime_index, &key, &val, MDB_NODUPDATA);
            if(rc && rc!=MDB_KEYEXIST){ success = FALSE; set_mdb_error(d,rc); }
        }
        for(size_t i=0; success && i<attr_count; ++i){
            MDB_val key = {.mv_data=&attr_entries[i].key, .mv_size=sizeof(uint32_t)};
            MDB_val val = {.mv_data=&attr_entries[i].value, .mv_size=sizeof(uint64_t)};
            rc = mdb_put(d->wtxn, d->dbi_attr_index, &key, &val, MDB_NODUPDATA);
            if(rc && rc!=MDB_KEYEXIST){ success = FALSE; set_mdb_error(d,rc); }
        }
        for(size_t i=0; success && i<ext_count; ++i){
            MDB_val key = {.mv_data=ext_entries[i].key, .mv_size=ext_entries[i].len};
            MDB_val val = {.mv_data=&ext_entries[i].value, .mv_size=sizeof(uint64_t)};
            rc = mdb_put(d->wtxn, d->dbi_extension_index, &key, &val, MDB_NODUPDATA);
            if(rc && rc!=MDB_KEYEXIST){ success = FALSE; set_mdb_error(d,rc); }
        }
        for(size_t i=0; success && i<content_count; ++i){
            MDB_val key = {.mv_data=&content_entries[i].key, .mv_size=sizeof(uint64_t)};
            MDB_val val = {.mv_data=&content_entries[i].value, .mv_size=sizeof(uint64_t)};
            rc = mdb_put(d->wtxn, d->dbi_content_index, &key, &val, MDB_NODUPDATA);
            if(rc && rc!=MDB_KEYEXIST){ success = FALSE; set_mdb_error(d,rc); }
        }
        for(size_t i=0; success && i<author_count; ++i){
            MDB_val key = {.mv_data=&author_entries[i].key, .mv_size=sizeof(uint64_t)};
            MDB_val val = {.mv_data=&author_entries[i].value, .mv_size=sizeof(uint64_t)};
            rc = mdb_put(d->wtxn, d->dbi_author_index, &key, &val, MDB_NODUPDATA);
            if(rc && rc!=MDB_KEYEXIST){ success = FALSE; set_mdb_error(d,rc); }
        }
        for(size_t i=0; success && i<camera_count; ++i){
            MDB_val key = {.mv_data=&camera_entries[i].key, .mv_size=sizeof(uint64_t)};
            MDB_val val = {.mv_data=&camera_entries[i].value, .mv_size=sizeof(uint64_t)};
            rc = mdb_put(d->wtxn, d->dbi_camera_index, &key, &val, MDB_NODUPDATA);
            if(rc && rc!=MDB_KEYEXIST){ success = FALSE; set_mdb_error(d,rc); }
        }
        for(size_t i=0; success && i<lens_count; ++i){
            MDB_val key = {.mv_data=&lens_entries[i].key, .mv_size=sizeof(uint64_t)};
            MDB_val val = {.mv_data=&lens_entries[i].value, .mv_size=sizeof(uint64_t)};
            rc = mdb_put(d->wtxn, d->dbi_lens_index, &key, &val, MDB_NODUPDATA);
            if(rc && rc!=MDB_KEYEXIST){ success = FALSE; set_mdb_error(d,rc); }
        }
        for(size_t i=0; success && i<artist_count; ++i){
            MDB_val key = {.mv_data=&artist_entries[i].key, .mv_size=sizeof(uint64_t)};
            MDB_val val = {.mv_data=&artist_entries[i].value, .mv_size=sizeof(uint64_t)};
            rc = mdb_put(d->wtxn, d->dbi_artist_index, &key, &val, MDB_NODUPDATA);
            if(rc && rc!=MDB_KEYEXIST){ success = FALSE; set_mdb_error(d,rc); }
        }
        for(size_t i=0; success && i<album_count; ++i){
            MDB_val key = {.mv_data=&album_entries[i].key, .mv_size=sizeof(uint64_t)};
            MDB_val val = {.mv_data=&album_entries[i].value, .mv_size=sizeof(uint64_t)};
            rc = mdb_put(d->wtxn, d->dbi_album_index, &key, &val, MDB_NODUPDATA);
            if(rc && rc!=MDB_KEYEXIST){ success = FALSE; set_mdb_error(d,rc); }
        }
        for(size_t i=0; success && i<title_count; ++i){
            MDB_val key = {.mv_data=&title_entries[i].key, .mv_size=sizeof(uint64_t)};
            MDB_val val = {.mv_data=&title_entries[i].value, .mv_size=sizeof(uint64_t)};
            rc = mdb_put(d->wtxn, d->dbi_title_index, &key, &val, MDB_NODUPDATA);
            if(rc && rc!=MDB_KEYEXIST){ success = FALSE; set_mdb_error(d,rc); }
        }
    }
    if(success){
        MDB_val mkv, mvv; const char* H="header"; to_mdb_val(H, strlen(H), &mkv);
        to_mdb_val(&header_tmp, sizeof(header_tmp), &mvv);
        rc = mdb_put(d->wtxn, d->dbi_meta, &mkv, &mvv, 0);
        if(rc){ success = FALSE; set_mdb_error(d, rc); }
    }

    d->wtxn = parent_txn;

    if(!success){
        mdb_txn_abort(batch_txn);
        LARGE_INTEGER li; li.QuadPart = (LONGLONG)bloom_offset_before;
        SetFilePointerEx(d->bloom_file, li, NULL, FILE_BEGIN);
        SetEndOfFile(d->bloom_file);
        d->bloom_file_size = bloom_file_size_before;
        d->bloom_buffer_len = bloom_buffer_len_before;
        d->header_cache = header_before;
        d->bloom_offset = bloom_offset_before;
        d->dirty = dirty_before;
        d->last_write_progress = processed;
        if(d->last_error.code == DB_ERROR_LMDB && d->last_error.detail == MDB_MAP_FULL){
            result = FALSE;
            goto cleanup;
        }
        Sleep(1);
        goto retry_batch;
    }

    rc = mdb_txn_commit(batch_txn);
    if(rc){
        set_mdb_error(d, rc);
        LARGE_INTEGER li; li.QuadPart = (LONGLONG)bloom_offset_before;
        SetFilePointerEx(d->bloom_file, li, NULL, FILE_BEGIN);
        SetEndOfFile(d->bloom_file);
        d->bloom_file_size = bloom_file_size_before;
        d->bloom_buffer_len = bloom_buffer_len_before;
        d->header_cache = header_before;
        d->bloom_offset = bloom_offset_before;
        d->dirty = dirty_before;
        d->last_write_progress = processed;
        if(d->last_error.code == DB_ERROR_LMDB && d->last_error.detail == MDB_MAP_FULL){
            result = FALSE;
            goto cleanup;
        }
        Sleep(1);
        goto retry_batch;
    }

    d->header_cache = header_tmp;
    d->bloom_offset = bloom_offset_tmp;
    d->dirty = TRUE;
    d->last_write_progress = count;
    set_error(d, DB_ERROR_NONE,0,NULL);
    result = TRUE;
    goto cleanup;

cleanup:
    free(fname_entries);
    free(parent_entries);
    free(path_entries);
    free(size_entries);
    free(date_entries);
    free(mtime_entries);
    free(attr_entries);
    free(ext_entries);
    free(content_entries);
    free(author_entries);
    free(camera_entries);
    free(lens_entries);
    free(artist_entries);
    free(album_entries);
    free(title_entries);
    return result;
}

static BOOL db_delete_record(DbImpl* d, uint64_t id, const DbRecord* r){
    MDB_val k;
    to_mdb_val(&id, sizeof(id), &k);
    mdb_del(d->wtxn, d->dbi_records, &k, NULL);

    uint64_t fname_key = r->normalized_name_str_id ? r->normalized_name_str_id : r->name_str_id;
    MDB_val ik,iv; to_mdb_val(&fname_key, sizeof(fname_key), &ik); to_mdb_val(&id, sizeof(id), &iv);
    mdb_del(d->wtxn, d->dbi_fname_index, &ik, &iv);

    MDB_val pk,pv; to_mdb_val(&r->parent_str_id, sizeof(r->parent_str_id), &pk); to_mdb_val(&id, sizeof(id), &pv);
    mdb_del(d->wtxn, d->dbi_parent_index, &pk, &pv);
    mdb_del(d->wtxn, d->dbi_path_hierarchy, &pk, &pv);

    if(r->type == DB_REC_FILE){
        MDB_val sk,sv; to_mdb_val(&r->file_size, sizeof(r->file_size), &sk); to_mdb_val(&id, sizeof(id), &sv);
        mdb_del(d->wtxn, d->dbi_size_index, &sk, &sv);
    }

    uint64_t day = filetime_days(r->modified_time);
    MDB_val dk,dv; to_mdb_val(&day, sizeof(day), &dk); to_mdb_val(&id, sizeof(id), &dv);
    mdb_del(d->wtxn, d->dbi_date_index, &dk, &dv);
    MDB_val mk,mv; to_mdb_val(&r->modified_time, sizeof(r->modified_time), &mk); to_mdb_val(&id, sizeof(id), &mv);
    mdb_del(d->wtxn, d->dbi_mtime_index, &mk, &mv);
    MDB_val ak,av; to_mdb_val(&r->attributes, sizeof(r->attributes), &ak); to_mdb_val(&id, sizeof(id), &av);
    mdb_del(d->wtxn, d->dbi_attr_index, &ak, &av);

    MDB_val namev;
    BOOL have_name = str_by_id_with_retry(d, r->name_str_id, &namev, 5, NULL, NULL);
    if(have_name){
        char ext[32]; split_extension_utf8((const char*)namev.mv_data, ext, sizeof(ext));
        if(ext[0]){
            MDB_val ek={.mv_data=ext,.mv_size=strlen(ext)}, ev={.mv_data=&id,.mv_size=sizeof(id)};
            mdb_del(d->wtxn, d->dbi_extension_index, &ek, &ev);
        }
    }
    MDB_val normv;
    if(r->normalized_name_str_id && str_by_id_with_retry(d, r->normalized_name_str_id, &normv, 5, NULL, NULL)){
        remove_trigrams(d, (const char*)normv.mv_data, r->normalized_name_str_id);
    } else if(have_name){
        remove_trigrams(d, (const char*)namev.mv_data, r->name_str_id);
    }

    if(r->content_str_id){
        MDB_val ck,cv; to_mdb_val(&r->content_str_id, sizeof(r->content_str_id), &ck); to_mdb_val(&id, sizeof(id), &cv);
        mdb_del(d->wtxn, d->dbi_content_index, &ck, &cv);
        MDB_val cvstr;
        if(str_by_id_with_retry(d, r->content_str_id, &cvstr, 5, NULL, NULL)){
            remove_trigrams(d, (const char*)cvstr.mv_data, r->content_str_id);
        }
    }

    if(r->author_str_id){ MDB_val ak,av; to_mdb_val(&r->author_str_id, sizeof(r->author_str_id), &ak); to_mdb_val(&id, sizeof(id), &av); mdb_del(d->wtxn, d->dbi_author_index, &ak, &av); }
    if(r->camera_str_id){ MDB_val ck,av; to_mdb_val(&r->camera_str_id, sizeof(r->camera_str_id), &ck); to_mdb_val(&id, sizeof(id), &av); mdb_del(d->wtxn, d->dbi_camera_index, &ck, &av); }
    if(r->lens_str_id){ MDB_val lk,av; to_mdb_val(&r->lens_str_id, sizeof(r->lens_str_id), &lk); to_mdb_val(&id, sizeof(id), &av); mdb_del(d->wtxn, d->dbi_lens_index, &lk, &av); }
    if(r->artist_str_id){ MDB_val ark,av; to_mdb_val(&r->artist_str_id, sizeof(r->artist_str_id), &ark); to_mdb_val(&id, sizeof(id), &av); mdb_del(d->wtxn, d->dbi_artist_index, &ark, &av); }
    if(r->album_str_id){ MDB_val abk,av; to_mdb_val(&r->album_str_id, sizeof(r->album_str_id), &abk); to_mdb_val(&id, sizeof(id), &av); mdb_del(d->wtxn, d->dbi_album_index, &abk, &av); }
    if(r->title_str_id){ MDB_val tk,av; to_mdb_val(&r->title_str_id, sizeof(r->title_str_id), &tk); to_mdb_val(&id, sizeof(id), &av); mdb_del(d->wtxn, d->dbi_title_index, &tk, &av); }
    return TRUE;
}

BOOL db_delete_path(Db* db_, const wchar_t* parent, const wchar_t* name){
    DbImpl* d = (DbImpl*)db_;
    if(!d->wtxn){ if(!db_begin_write(db_)) return FALSE; }
    d->dirty = TRUE;
    uint64_t parent_id = db_intern_wstring(db_, parent);
    const wchar_t* name_array[1] = { name };
    uint8_t need_norm[1] = { 1 };
    uint64_t name_ids[1] = { 0 };
    uint64_t norm_ids[1] = { 0 };
    db_intern_wstrings_batched(db_, name_array, need_norm, 1, name_ids, norm_ids);
    uint64_t name_id = name_ids[0];
    uint64_t norm_id = norm_ids[0] ? norm_ids[0] : name_id;
    if(!parent_id || !name_id) return TRUE;
    MDB_cursor* cur;
    MDB_val key={.mv_data=&norm_id,.mv_size=sizeof(norm_id)}, val;
    int rc = mdb_cursor_open(d->wtxn, d->dbi_fname_index, &cur);
    if(rc){ set_mdb_error(d, rc); return FALSE; }
    rc = mdb_cursor_get(cur, &key, &val, MDB_SET);
    while(rc==0){
        uint64_t id = *(uint64_t*)val.mv_data;
        MDB_val rk,rv; to_mdb_val(&id,sizeof(id),&rk);
        if(mdb_get(d->wtxn, d->dbi_records, &rk, &rv)==0){
            DbRecord r; memcpy(&r, rv.mv_data, sizeof(r));
            if(r.parent_str_id == parent_id && r.name_str_id == name_id){
                db_delete_record(d, id, &r);
                break;
            }
        }
        rc = mdb_cursor_get(cur, &key, &val, MDB_NEXT_DUP);
    }
    mdb_cursor_close(cur);
    return TRUE;
}

BOOL db_get_record_by_path(Db* db_, const wchar_t* parent, const wchar_t* name, DbRecord* out){
    DbImpl* d = (DbImpl*)db_;
    MDB_txn* txn = d->wtxn;
    BOOL own_txn = FALSE;
    if(!txn){
        if(mdb_txn_begin(d->env, NULL, MDB_RDONLY, &txn)!=0) return FALSE;
        own_txn = TRUE;
    }
    uint64_t parent_id = db_intern_wstring(db_, parent);
    const wchar_t* name_array[1] = { name };
    uint8_t need_norm[1] = { 1 };
    uint64_t name_ids[1] = { 0 };
    uint64_t norm_ids[1] = { 0 };
    db_intern_wstrings_batched(db_, name_array, need_norm, 1, name_ids, norm_ids);
    uint64_t name_id = name_ids[0];
    uint64_t norm_id = norm_ids[0] ? norm_ids[0] : name_id;
    if(!parent_id || !name_id){ if(own_txn) mdb_txn_abort(txn); return FALSE; }
    MDB_cursor* cur;
    MDB_val key={.mv_data=&norm_id,.mv_size=sizeof(norm_id)}, val;
    int rc = mdb_cursor_open(txn, d->dbi_fname_index, &cur);
    if(rc){ if(own_txn) mdb_txn_abort(txn); return FALSE; }
    rc = mdb_cursor_get(cur, &key, &val, MDB_SET);
    BOOL found = FALSE;
    while(rc==0){
        uint64_t id = *(uint64_t*)val.mv_data;
        MDB_val rk,rv; to_mdb_val(&id,sizeof(id),&rk);
        if(mdb_get(txn, d->dbi_records, &rk, &rv)==0){
            DbRecord* r=(DbRecord*)rv.mv_data;
            if(r->parent_str_id == parent_id && r->name_str_id == name_id){
                if(out) *out = *r;
                found = TRUE;
                break;
            }
        }
        rc = mdb_cursor_get(cur, &key, &val, MDB_NEXT_DUP);
    }
    mdb_cursor_close(cur);
    if(own_txn) mdb_txn_abort(txn);
    return found;
}
