# FIX LOG — Pass 1: Memory Safety & Buffer Handling

## Summary
| Metric | Value |
|--------|-------|
| Findings fixed | 7 / 7 addressable |
| Files modified | 7 |
| Build result | PASS (0 errors) |
| Test result | PASS (7/7 tests) |
| Regressions | None |

## Fixes Applied

### P1-001 — NULL check after malloc in prog_submit (HIGH)
- **File:** `src/core/search.c:342`
- **Fix:** Added `if(!h) continue;` after `malloc(sizeof(ProgHit))` to skip the entry on OOM instead of dereferencing NULL.
- **Verified:** Build + test pass.

### P1-002 — Integer overflow in curl_write_cb (MEDIUM)
- **Files:** `src/services/cloud.c`, `plugins/data_sources/gmail_plugin.c`, `plugins/data_sources/icloud_plugin.c`, `plugins/data_sources/microsoft_mail_plugin.c`
- **Fix:** Added two overflow guards before multiplication and addition:
  - `if(size && nmemb > SIZE_MAX / size) return 0;`
  - `if(realsize > SIZE_MAX - mem->size - 1) return 0;`
- **Verified:** Build + test pass.

### P1-003 — Unchecked malloc for prog_submit array (MEDIUM)
- **File:** `src/core/search.c` (lines ~631, ~741, ~786)
- **Fix:** Wrapped `malloc(n*sizeof(uint32_t))` and subsequent `prog_submit` + `free` calls in `if(out){...}` blocks to skip gracefully on allocation failure.
- **Verified:** Build + test pass.

### P1-004 — IdMap malloc failure crashes (MEDIUM)
- **File:** `src/core/search.c`
- **Fix:**
  - Changed `idmap_init` return type from `void` to `BOOL` with NULL checks on both `malloc` calls; frees on partial failure.
  - Changed `idmap_grow` return type from `void` to `BOOL`; allocates into temporaries before assigning, frees on failure.
  - Changed `idmap_set` return type from `void` to `BOOL`; propagates `idmap_grow` failure.
  - Updated `prog_state_init` to return `BOOL` and check `idmap_init` result, rolling back `InitializeCriticalSection` on failure.
- **Verified:** Build + test pass.

### P1-005 — strcpy/strncpy buffer overflow in network.c (MEDIUM)
- **File:** `src/services/network.c` (lines 85-91 and 178-184)
- **Fix:** Replaced unbounded `strcpy` with `strncpy` + explicit null terminator; replaced `strncpy` with bounded `memcpy` + explicit length clamping against `sizeof(parent)`.
- **Verified:** Build + test pass.

### P1-006 — Integer underflow in EXIF seglen (MEDIUM)
- **File:** `src/services/metadata.c:58-59`
- **Fix:** Added `seglen >= 10` guard before computing `seglen - 8` (the Exif header after the marker is 10 bytes: 2 marker + 2 length + 6 "Exif\0\0"). Prevents underflow when a malformed JPEG has a truncated APP1 segment.
- **Verified:** Build + test pass.

### P1-007 — tokenlist_push realloc NULL dereference (LOW)
- **File:** `src/core/search.c:504`
- **Fix:** Changed `tokenlist_push` to allocate into a temporary `Token* p` and check for NULL before updating `t->items` and `t->cap`. Changed return type to `BOOL`.
- **Verified:** Build + test pass.

## Deferred / Won't Fix
- P1-008 through P1-011 (Low severity): scanner_win.cpp `_snwprintf` and plugin `mbstowcs` issues — these are bounded by MAX_PATH/MAX_LONG_PATH destination buffers and represent minimal risk. Will revisit if surfaced in later passes.
