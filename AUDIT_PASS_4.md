# AUDIT PASS 4 — Concurrency

## Focus
Thread safety, lock-free data structures, race conditions, deadlocks, atomics correctness.

## Findings

### P4-001 — NULL dereference in prog_mark_done/prog_submit (HIGH)
- **File:** `src/core/search.c:342, 361`
- **Description:** Both functions dereference their ProgState pointer without NULL check. If called with g_prog_state=NULL (e.g., early error path), they crash on EnterCriticalSection.

### P4-009 — Bloom cache returns pointer after releasing lock (MEDIUM)
- **File:** `src/core/search.c:142-144`
- **Description:** bloom_cache_get releases the critical section before returning a pointer to cached data. Another thread could evict and realloc that entry, invalidating the pointer.

### P4-004 — MPMC on_push callback race (MEDIUM)
- **File:** `src/core/mpmc_queue.c:60-62, 98-102`
- **Description:** MPMC_SetOnPush writes callback pointers without synchronization while MPMC_Push reads them concurrently.

### P4-006 — CancelToken weak ordering on non-x86 (LOW)
- **File:** `include/anything/anything.h:33-39`
- **Description:** is_cancelled reads volatile BOOL without acquire barrier. Correct on x86 (TSO), may be stale on ARM.

### P4-011 — Plugin global state unsynchronized (MEDIUM)
- **File:** `src/system/plugin.c:17-45`
- **Description:** g_plugins array and g_plugin_count written without lock. Safe if Plugin_LoadAll completes before Plugin_ScanAll, but not enforced.

## Summary
| Severity | Count |
|----------|-------|
| Critical | 0 |
| High     | 1 |
| Medium   | 3 |
| Low      | 1 |
| Total    | 5 |
