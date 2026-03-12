# FIX LOG — Pass 8: Protocol + Integration

## Summary
| Metric | Value |
|--------|-------|
| Findings fixed | 1 / 5 |
| Files modified | 1 |
| Build result | PASS (0 errors) |
| Test result | PASS (7/7 tests) |
| Regressions | None |

## Fixes Applied

### P8-002 — EXIF IFD pointer bounds validation (MEDIUM)
- **File:** `src/services/metadata.c:73-74`
- **Fix:** Added `if(len < 8) return;` to ensure there's enough data to read the IFD0 offset, and `if(ifd0 >= len) return;` to validate the IFD pointer doesn't exceed the EXIF segment boundary. Prevents out-of-bounds reads from crafted JPEG files.
- **Verified:** Build + test pass.

## Deferred
- P8-001 (TLS verification): libcurl defaults are correct. Explicit set would be belt-and-suspenders.
- P8-003 (XXE): Custom parser doesn't process entities — not vulnerable.
- P8-004 (strcpy static strings): Cosmetic warning fix, not a security issue.
- P8-005 (cJSON depth): Library has built-in protection.
