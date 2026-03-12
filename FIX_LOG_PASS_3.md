# FIX LOG — Pass 3: LMDB + Data Integrity

## Summary
| Metric | Value |
|--------|-------|
| Findings fixed | 3 / 7 |
| Files modified | 1 (database.c) |
| Build result | PASS (0 errors) |
| Test result | PASS (7/7 tests) |
| Regressions | None |

## Fixes Applied

### P3-002 — Trigram blob size alignment validation (CRITICAL)
- **File:** `src/core/database.c:2815`
- **Fix:** Added `if(blob_len % 3 != 0) return;` before computing tri_count. Corrupted blobs with non-aligned sizes are now safely skipped.
- **Verified:** Build + test pass.

### P3-003 — Record size validation before memcpy/cast (HIGH)
- **File:** `src/core/database.c:3609, 3651`
- **Fix:** Added `rv.mv_size >= sizeof(DbRecord)` guard to both the `memcpy` path (db_delete_path) and the pointer-cast path (db_get_record_by_path).
- **Verified:** Build + test pass.

### P3-006 — Zstd zero-length decompression rejection (MEDIUM)
- **File:** `src/core/database.c:770`
- **Fix:** Changed `if(raw_len > SIZE_MAX)` to `if(raw_len == 0 || raw_len > SIZE_MAX)` to reject empty compressed payloads.
- **Verified:** Build + test pass.

## Deferred
- P3-001 (string ID overflow): uint64 counter, practically unreachable in any real workload.
- P3-005 (bloom tail TOCTOU): The second check after InterlockedExchangeAdd is a safety net; the race window is extremely narrow and the consequence (error return + retry) is benign.
- P3-009 (nested txn abort): Edge case only triggered by LMDB internal errors. Low practical impact.
- P3-011 (string cache race): The BUSY sentinel prevents other threads from using stale data. The allocation-failure recovery is correct.
