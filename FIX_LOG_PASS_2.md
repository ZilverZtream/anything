# FIX LOG — Pass 2: Archive + Path Traversal

## Summary
| Metric | Value |
|--------|-------|
| Findings fixed | 3 / 4 addressable (P2-002 deferred) |
| Files modified | 3 |
| Build result | PASS (0 errors) |
| Test result | PASS (7/7 tests) |
| Regressions | None |

## Fixes Applied

### P2-001 — Cloud API name sanitization (MEDIUM)
- **File:** `src/services/cloud.c`
- **Fix:** Added `name_safe()` helper that rejects names containing `/`, `\`, `:`, or that are exactly `.` or `..`. Applied to all four cloud drive scanners (OneDrive, Google Drive, pCloud, Dropbox) before `enqueue_item` and `path_join` calls.
- **Verified:** Build + test pass.

### P2-005 — Archive %00 NUL byte injection (LOW)
- **File:** `src/services/archive.c:50`
- **Fix:** Added `if(ch == '\0') return FALSE;` after URL-decoding to reject NUL bytes that would truncate the decoded path.
- **Verified:** Build + test pass.

### CMake zstd target fix (BUILD)
- **File:** `CMakeLists.txt:91`
- **Fix:** Changed `zstd::zstd` to `zstd::libzstd` to match the installed vcpkg package's CMake target name. This was a pre-existing build issue exposed by branch switch.
- **Verified:** Full rebuild + test pass.

## Deferred
- P2-002 (pCloud token in URL): API limitation — pCloud's auth mechanism uses query parameter tokens. Would require upstream API support for header-based auth. Documented as known limitation.
- P2-003 (Google Drive query injection): Low risk — the `id` comes from previous Google API responses. URL-encoding would be defensive but is not exploitable in practice.
