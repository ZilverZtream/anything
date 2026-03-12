# AUDIT PASS 3 — LMDB + Data Integrity

## Focus
LMDB transaction safety, data serialization bounds, Zstd compression, bloom filter correctness, string encoding, trigram storage.

## Findings

### P3-002 — Unvalidated trigram blob size alignment (CRITICAL)
- **File:** `src/core/database.c:2815-2816`
- **Description:** Trigram blobs are read from LMDB and divided by 3 to get tri_count, but the blob size is not validated to be a multiple of 3. Remainder bytes indicate corruption and could lead to reading partial trigrams.

### P3-003 — Missing record size validation in memcpy/cast (HIGH)
- **File:** `src/core/database.c:3609, 3651`
- **Description:** DbRecord structs are read from LMDB via memcpy or pointer cast without checking that `rv.mv_size >= sizeof(DbRecord)`. A corrupted or truncated value causes out-of-bounds read.

### P3-006 — Zstd decompression accepts zero-length raw_len (MEDIUM)
- **File:** `src/core/database.c:770`
- **Description:** If the compressed header stores raw_len=0, the code allocates a zero-byte buffer and passes it to ZSTD_decompress, which could succeed with 0 bytes decompressed. This should be rejected as invalid.

### P3-001 — String ID overflow (MEDIUM)
- **File:** `src/core/database.c:2277-2278`
- **Description:** string_count is incremented without overflow check. At 2^64, IDs wrap around causing duplicate string IDs. Practically unreachable but a correctness concern.

### P3-005 — TOCTOU in bloom tail atomic (MEDIUM)
- **File:** `src/core/database.c:867-918`
- **Description:** Between reading bloom_tail and atomically adding to it, another thread could advance the tail, potentially causing the overflow check to pass on stale data.

### P3-009 — Missing txn abort on nested error path (MEDIUM)
- **File:** `src/core/database.c:3074-3092`
- **Description:** After aborting parent_txn on MDB_BAD_TXN, if the retry also fails, error handling doesn't fully clean up.

### P3-011 — String cache race with allocation failure (MEDIUM)
- **File:** `src/core/database.c:204-214`
- **Description:** Between CAS setting BUSY and _strdup, allocation failure leaves a brief window where another thread could see stale data.

## Summary
| Severity | Count |
|----------|-------|
| Critical | 1 |
| High     | 1 |
| Medium   | 5 |
| Low      | 0 |
| Total    | 7 |
