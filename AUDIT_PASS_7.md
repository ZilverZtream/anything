# AUDIT PASS 7 — Platform-Specific

## Focus
NTFS USN journal, Windows API usage, Linux inotify/nftw, macOS CoreServices, SIMD alignment, cross-platform compatibility.

## Findings

### P7-001 — NTFS USN buffer alignment not guaranteed (LOW)
- **File:** `src/platform/ntfs.c`
- **Description:** The USN record buffer is heap-allocated via malloc. While Windows aligns heap allocations to at least 8 bytes, USN_RECORD_V3 may benefit from 16-byte alignment for performance. No correctness issue.
- **Status:** Low risk.

### P7-002 — scanner_win.cpp _snwprintf truncation (LOW)
- **File:** `src/platform/scanner_win.cpp:169`
- **Description:** `_snwprintf` may truncate the path without null termination if the buffer is full. However, the destination is MAX_PATH which matches the Windows API limit.
- **Status:** Low risk — paths longer than MAX_PATH use the \\?\ prefix elsewhere.

### P7-003 — Linux scanner wcstombs no null termination check (MEDIUM)
- **File:** `src/platform/scanner_linux.c:138, 154`
- **Description:** `wcstombs(tmp, rootPath, PATH_MAX)` doesn't guarantee null termination if the conversion fills the buffer exactly. Should add explicit `tmp[PATH_MAX-1]=0`.

### P7-004 — macOS APFS scanner missing error check on ext2fs_dir_iterate2 (LOW)
- **File:** `src/platform/wsl.c:197`
- **Description:** The return value of `ext2fs_dir_iterate2` is checked but error details are not logged.
- **Status:** Low risk.

### P7-005 — SIMD fallback paths correct (PASS)
- **File:** `src/core/util.c`
- **Description:** SIMD search paths (AVX2, SSE4.1) check CPUID before use and fall back to scalar. Alignment is ensured by aligned_malloc.
- **Status:** **PASS**

## Summary
| Severity | Count |
|----------|-------|
| Critical | 0 |
| High     | 0 |
| Medium   | 1 |
| Low      | 3 |
| Pass     | 1 |
| Total    | 4 |
