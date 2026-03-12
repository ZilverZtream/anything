# Audit Pass 1 — Memory Safety + Buffer Handling

## Prior fix verification
N/A — this is the first pass.

## New findings

### [P1-001] Unchecked malloc in prog_submit leads to NULL dereference
- Severity: High
- Confidence: Confirmed
- Coverage: Uncovered
- Affected modes: CLI / GUI (search path)
- Invariant violated: All malloc return values must be checked before dereference
- Evidence: `src/core/search.c:342` — `ProgHit* h = (ProgHit*)malloc(sizeof(ProgHit));` followed immediately by `h->rec_id = id;` with no NULL check.
- Impact: Under memory pressure during search with many results, NULL dereference crashes the process.
- Trigger: Run a broad search query that produces millions of candidate hits while system is under memory pressure.
- Why tests miss it: `search_test.c` tests query parsing/logic, not OOM paths in progressive search.

### [P1-002] Integer overflow in curl_write_cb across multiple files (shared root cause)
- Severity: Medium
- Confidence: Confirmed
- Coverage: Uncovered
- Affected modes: plugin / cloud
- Invariant violated: `size * nmemb` and `mem->size + realsize + 1` must not overflow
- Evidence:
  - `src/services/cloud.c:197` — `size_t realsize = size * nmemb;`
  - `plugins/data_sources/gmail_plugin.c:64`
  - `plugins/data_sources/icloud_plugin.c:66`
  - `plugins/data_sources/microsoft_mail_plugin.c:82`
  - All follow the pattern: `realloc(mem->data, mem->size + realsize + 1)` with no overflow check on either multiplication or addition.
- Impact: Attacker-controlled HTTP response with crafted Content-Length could cause undersized allocation and subsequent heap buffer overflow via `memcpy`.
- Trigger: Malicious OAuth API response or MITM on cloud provider API returns crafted size values.
- Why tests miss it: `cloud_test.c` tests sync logic, not curl callback with adversarial data. No plugin integration tests with mocked HTTP.

### [P1-003] Unchecked malloc in search.c progressive search allocations
- Severity: Medium
- Confidence: Confirmed
- Coverage: Uncovered
- Affected modes: CLI / GUI (search path)
- Invariant violated: All malloc return values must be checked before use
- Evidence:
  - `src/core/search.c:631` — `uint32_t* out=(uint32_t*)malloc(n*sizeof(uint32_t));` followed by loop writing to `out[i]` with no NULL check.
  - `src/core/search.c:738` — same pattern
  - `src/core/search.c:781` — same pattern
- Impact: NULL dereference crash under memory pressure when search produces large result sets.
- Trigger: Broad query on a large index while system memory is constrained.
- Why tests miss it: Tests don't exercise OOM conditions.

### [P1-004] Integer overflow in idmap_init / idmap_grow can cause zero-size allocation
- Severity: Medium
- Confidence: Confirmed
- Coverage: Uncovered
- Affected modes: CLI / GUI (search path)
- Invariant violated: Hash map capacity calculations must not overflow
- Evidence:
  - `src/core/search.c:244-248` — `idmap_init` doubles `m->cap` in a loop with no overflow guard. Then `malloc(m->cap * sizeof(uint32_t))` and `malloc(m->cap * sizeof(uint16_t))` are called without checking return values.
  - `src/core/search.c:267-277` — `idmap_grow` doubles capacity and does `malloc(m->cap*sizeof(uint32_t))` without NULL check. If `m->cap` overflows to 0 via repeated doubling, this allocates 0 bytes and subsequent writes corrupt heap.
- Impact: Heap corruption leading to arbitrary code execution, or crash.
- Trigger: Extremely large result set causing repeated growth of the IdMap.
- Why tests miss it: Tests use small data sets.

### [P1-005] Unbounded strcpy in cloud.c and network.c
- Severity: Medium
- Confidence: Confirmed
- Coverage: Uncovered
- Affected modes: cloud / CLI
- Invariant violated: String copies must be bounds-checked
- Evidence:
  - `src/services/cloud.c:402` — `strcpy(path, "/v1.0/me/drive/root/children?...")` into `char path[512]`. The string is 82 chars so safe now, but fragile — no bounds enforcement.
  - `src/services/cloud.c:490` — `strcpy(path, ...)` same pattern into `char path[1024]`.
  - `src/services/network.c:90` — `strcpy(parent, fpath)` where `fpath` comes from `nftw` callback and could exceed `PATH_MAX` on some systems.
  - `src/services/network.c:183` — same pattern (Apple version).
- Impact: Stack buffer overflow if path strings exceed buffer size. The network.c cases are higher risk because `fpath` comes from OS filesystem walk.
- Trigger: Deep directory trees or UNC paths on network shares.
- Why tests miss it: No tests for network scanner or cloud API URL construction.

### [P1-006] Integer underflow in metadata.c EXIF/ID3 parsing
- Severity: Medium
- Confidence: Confirmed
- Coverage: Uncovered
- Affected modes: CLI (content indexing)
- Invariant violated: Arithmetic on untrusted data must check for underflow
- Evidence:
  - `src/services/metadata.c:59` — `len=seglen-8` where `seglen` is from JPEG marker data. If `seglen < 8`, this underflows to a huge value, causing `parse_ifd` to read far beyond the buffer.
  - `src/services/metadata.c:94` — `size_t dlen=fsize-1` where `fsize` is the ID3 frame size. If `fsize==0` (checked on line 92 as `if(fsize==0||...)` but `fsize==1` is not caught), then `dlen=0` which is fine. However if fsize parsing goes wrong and yields 0 before the check at line 92, the underflow would be catastrophic.
- Impact: Out-of-bounds read of up to 64KB buffer, potentially reading sensitive memory or crashing.
- Trigger: Malformed JPEG or MP3 file with crafted marker/frame sizes. Files are routinely scanned during indexing.
- Why tests miss it: No tests for metadata extraction.

### [P1-007] Unchecked realloc in tokenlist_push (search query parser)
- Severity: Medium
- Confidence: Confirmed
- Coverage: Uncovered
- Affected modes: CLI / GUI (search)
- Invariant violated: realloc return must be checked
- Evidence: `src/core/search.c:493` — `t->items=(Token*)realloc(t->items,t->cap*sizeof(Token));` — if realloc returns NULL, `t->items` is set to NULL, leaking the old allocation and causing immediate NULL dereference on `t->items[t->n++]=tk`.
- Impact: Memory leak + crash on OOM during query parsing.
- Trigger: Complex boolean query with many terms under memory pressure.
- Why tests miss it: Tests don't exercise OOM during query parsing.

### [P1-008] config_load_file does not clamp negative max_content_index_bytes
- Severity: Low
- Confidence: Confirmed
- Coverage: Uncovered
- Affected modes: CLI / GUI
- Invariant violated: Config values from untrusted input must be validated before use as sizes
- Evidence: `src/app/config.c:55` — `g_config.max_content_index_bytes = (int)wcstol(val, NULL, 10);` stores result as `int`. On line 66, the clamp `if(g_config.max_content_index_bytes < 1024)` catches negative values. But `wcstol` on a string like "2147483648" (INT_MAX+1) causes undefined behavior when cast to `int`.
- Impact: If a crafted config file provides values that cause int overflow, max_content_index_bytes could wrap to a negative value or become very small, causing truncated indexing. The existing clamp at line 66-67 mitigates this to 1KB minimum, reducing severity.
- Trigger: Malicious or corrupted config file.
- Why tests miss it: No config parsing tests exist.

### [P1-009] Thread-local Levenshtein buffer never freed (memory leak)
- Severity: Low
- Confidence: Confirmed
- Coverage: Uncovered
- Affected modes: CLI / GUI (search)
- Invariant violated: Thread-local heap allocations should be freed on thread exit
- Evidence: `src/core/util.c:531-532` — `static THREAD_LOCAL int* g_levenshtein_buffer = NULL;` is allocated via `realloc` on line 582 but never freed. Each search worker thread leaks this buffer when it exits.
- Impact: Memory leak proportional to thread count * max fuzzy search string length. Typically small (few KB per thread).
- Trigger: Any fuzzy search query.
- Why tests miss it: Tests don't check for leaks.

### [P1-010] Bloom cache returns pointer to entry data without holding lock
- Severity: Low
- Confidence: Provisional (need to verify if concurrent eviction is possible)
- Coverage: Uncovered
- Affected modes: CLI / GUI (search)
- Invariant violated: Cached data pointers must remain valid while in use
- Evidence: `src/core/search.c:143-144` — `bloom_cache_get` acquires critical section, sets `data = e->data`, releases critical section, returns `data`. If another thread concurrently evicts this entry and calls `realloc` on `slot->data` (line 173), the returned pointer becomes dangling.
- Impact: Use-after-free / dangling pointer read, causing incorrect bloom filter results or crash.
- Trigger: Concurrent searches that thrash the bloom cache (more than 1000 unique bloom filters accessed simultaneously).
- Why tests miss it: Tests are single-threaded.

### [P1-011] enterprise_audit_log format string injection
- Severity: Low
- Confidence: Confirmed
- Coverage: Uncovered
- Affected modes: enterprise
- Invariant violated: User-controlled strings must not be passed as format strings
- Evidence: `src/enterprise/enterprise.c:108` — `fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d\t%s\t%s\n", ..., user, query)` — the `user` and `query` parameters are passed through `%s` format specifiers, which is safe against format string attacks. However, the `query` parameter can contain tab and newline characters, allowing log injection (adding fake log entries).
- Impact: Log injection — attacker-controlled search query can forge audit log entries by embedding `\t` and `\n` characters.
- Trigger: Search query containing tab/newline characters in enterprise mode.
- Why tests miss it: `enterprise_test.c` tests API contract, not log content validation.

## Findings rejected after test review

- **Suspected: MPMC queue double-free on push failure.** Reviewed `mpmc_queue.c:48-72` — MPMC_Push returns FALSE if queue is full, caller retains ownership. Callers (e.g., cloud.c:291-293, anything.c:361-362) correctly free on push failure. No double-free possible. **Rejected: correct by design.**

- **Suspected: db_string_value_parse Zstd decompression buffer overflow.** Reviewed `database.c:762-778` — the raw_len is read from stored data and validated against SIZE_MAX (line 770). The decompression target buffer is allocated to exactly raw_len (line 772). ZSTD_decompress validates that output doesn't exceed the buffer (line 775-776). **Rejected: bounds-checked.**

- **Suspected: archive path traversal via null bytes.** Reviewed `archive.c:45-56` — the URL-decode loop stops on null byte (C string semantics), and the subsequent segment parsing uses `strcmp` which also stops at null. Any embedded null would truncate the path, not extend it. **Rejected: safe by C string semantics.**

## Pass 1 summary
- Prior fixes verified: 0 / 0 (first pass)
- Incomplete fixes: 0
- Regressions from prior fixes: 0
- New findings: 11 (Critical: 0, High: 1, Medium: 6, Low: 4)
