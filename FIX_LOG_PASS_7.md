# FIX LOG — Pass 7: Platform-Specific

## Summary
| Metric | Value |
|--------|-------|
| Findings fixed | 1 / 4 |
| Files modified | 1 |
| Build result | PASS (0 errors) |
| Test result | PASS (7/7 tests) |
| Regressions | None |

## Fixes Applied

### P7-003 — wcstombs null termination (MEDIUM)
- **File:** `src/platform/scanner_linux.c:138, 155`
- **Fix:** Added explicit `tmp[PATH_MAX-1]=0` and `s->root[PATH_MAX-1]=0` after each wcstombs call to ensure null termination when the conversion fills the entire buffer.
- **Verified:** Build + test pass.

## Deferred
- P7-001 (USN alignment): No correctness issue, only potential perf optimization.
- P7-002 (_snwprintf): MAX_PATH limitation is inherent to Windows APIs at this point.
- P7-004 (ext2fs error logging): Enhancement, not a security fix.
