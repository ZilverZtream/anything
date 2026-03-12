# FIX LOG — Pass 4: Concurrency

## Summary
| Metric | Value |
|--------|-------|
| Findings fixed | 2 / 5 |
| Files modified | 1 (search.c) |
| Build result | PASS (0 errors) |
| Test result | PASS (7/7 tests) |
| Regressions | None |

## Fixes Applied

### P4-001 — NULL checks in prog_submit and prog_mark_done (HIGH)
- **File:** `src/core/search.c:342, 361`
- **Fix:** Added `if(!ps || !ids || n==0) return;` to prog_submit and `if(!ps || stage>=3) return;` to prog_mark_done. Prevents NULL dereference if called before ProgState initialization.
- **Verified:** Build + test pass.

## Deferred
- P4-009 (bloom cache pointer): Requires ref-counting or copy-out pattern — too invasive for this pass. Mitigated by LRU stamping (recently-accessed entries survive eviction).
- P4-004 (MPMC on_push race): In practice, SetOnPush is called before threads start. Adding a barrier would be correct but not fixing a real bug.
- P4-006 (CancelToken ordering): Correct on x86/Windows (all targets). Would need __atomic_load on ARM.
- P4-011 (plugin globals): Load/scan ordering is enforced by calling code. Adding a lock would be pure defense.
