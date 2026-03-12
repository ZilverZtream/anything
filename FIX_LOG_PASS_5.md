# FIX LOG — Pass 5: Plugin System

## Summary
| Metric | Value |
|--------|-------|
| Findings fixed | 2 / 5 |
| Files modified | 2 |
| Build result | PASS (0 errors) |
| Test result | PASS (7/7 tests) |
| Regressions | None |

## Fixes Applied

### P5-002 — sprintf replaced with snprintf in code_plugin (MEDIUM)
- **File:** `plugins/code/code_plugin.c:35`
- **Fix:** Changed `sprintf(*buf + *len, ...)` to `snprintf(*buf + *len, *cap - *len, ...)` for bounds-safe formatting even though the preceding capacity check makes overflow unlikely.
- **Verified:** Build + test pass.

### P5-004 — strdup NULL check in web_archive_plugin (MEDIUM)
- **File:** `plugins/data_sources/web_archive_plugin.c:101`
- **Fix:** Added NULL check on strdup return. Now returns early without incrementing g_seen_count if allocation fails.
- **Verified:** Build + test pass.

## Deferred
- P5-003 (git integer overflow): Cumulative size is informational, not used for allocation. Low impact.
- P5-005 (registry malloc): Small fixed-size allocations with low failure probability.
- P5-006 (DLL search order): Plugin dir is alongside the exe — same security context.
