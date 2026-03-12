# Anything Security & Correctness Audit

You are auditing the Anything repository for critical correctness bugs, safety failures, and operational maturity.

## System under audit

Anything is a high-performance cross-platform desktop search engine written in C/C++ (C11/C++17, ~25,000 LoC) with:
- A multi-threaded indexing engine (LMDB-backed, trigram indices, bloom filters, Zstd compression)
- Platform-specific filesystem scanners (NTFS USN journal tailing, Linux inotify, macOS APFS/CoreServices)
- A dynamic plugin system (code indexing, git history, Gmail/Microsoft/iCloud email via OAuth 2.0, web archive, OCR, duplicate detection, Windows Registry)
- An ImGui-based GUI with Direct2D rendering and result virtualization
- Enterprise features (Active Directory auth, network share indexing, audit logging, permission checking — gated behind ENTERPRISE compile flag)
- Cloud sync capabilities (SharedIndex upload/download, team_id scoping)
- Lock-free MPMC work queues, adaptive thread pools (1–64 threads), SIMD-accelerated search (AVX2/SSE4.1)
- Archive content extraction (ZIP, TAR, 7Z, RAR via libarchive) with path traversal defenses
- Unit tests (database, search, util) and CI via GitHub Actions

## Audit objective

Deliver a deep, code-evidence-backed audit of correctness, memory safety, security boundaries, and operational trustworthiness under realistic usage across all execution modes: CLI indexer, GUI search, plugin-driven indexing, enterprise/network mode, and cloud sync mode.

---
## Hard constraints

1. NO CODE CHANGES — audit-only.
2. Code-path evidence only — no docs-only or comment-only claims.
3. Every claim must cite file + line — vague references are not findings.
4. Exhaustive subsystem coverage — do not stop at a fixed finding count.
5. Distinguish execution modes — a bug in enterprise mode may not affect standalone CLI.
6. Deduplicate across root causes — if two symptoms share a root cause, report one finding with both impacts.

## Severity classification

┌──────────┬───────────────────────────────────────────────────────────────────────────┬───────────────────────────────────────────────────────────┐
│  Level   │                                Definition                                │                          Example                          │
├──────────┼───────────────────────────────────────────────────────────────────────────┼───────────────────────────────────────────────────────────┤
│ Critical │ Arbitrary code execution, heap/stack corruption exploitable for RCE,     │ Buffer overflow in content parser, archive path traversal │
│          │ complete auth bypass, data exfiltration, silent data corruption           │ writing outside repo, HMAC bypass in cloud sync           │
├──────────┼───────────────────────────────────────────────────────────────────────────┼───────────────────────────────────────────────────────────┤
│ High     │ Significant safety/correctness failure under realistic conditions         │ TOCTOU in credential file, use-after-free in plugin       │
│          │                                                                           │ unload, integer overflow in index math                    │
├──────────┼───────────────────────────────────────────────────────────────────────────┼───────────────────────────────────────────────────────────┤
│ Medium   │ Correctness issue requiring specific conditions or causing degraded (not  │ Race in concurrent LMDB writes, missing null check on     │
│          │ wrong) behavior                                                           │ malloc return, config value not clamped                   │
├──────────┼───────────────────────────────────────────────────────────────────────────┼───────────────────────────────────────────────────────────┤
│ Low      │ Minor correctness gap, defense-in-depth improvement, or edge-case-only   │ Informational leak in debug output, missing validation    │
│          │ issue                                                                     │ on internal-only path, unchecked snprintf truncation      │
└──────────┴───────────────────────────────────────────────────────────────────────────┴───────────────────────────────────────────────────────────┘

Mark confidence: Confirmed (full evidence chain) or Provisional (evidence path incomplete — state what's missing).

---
## Audit strategy guidance

### Think like an attacker, verify like an engineer

For each subsystem, ask:
1. What invariant must hold? (e.g., "archive extraction never writes outside the target directory")
2. What breaks that invariant? (e.g., symlink in archive, ".." traversal, null byte injection, case folding)
3. Is the defense complete? (check all code paths, not just the happy path)
4. Do tests actually verify the invariant or just the mechanism?

### Prioritize interaction bugs over isolated bugs

The highest-value findings are at boundaries between subsystems:
- Plugin ↔ host data exchange (work queue items, cancel token propagation, string ownership)
- Scanner ↔ database writes (batch commit boundaries, partial scan recovery, string interning races)
- Archive extraction ↔ content indexing (path validation completeness, nested archives, symlink targets)
- Cloud API responses ↔ JSON parsing (malformed responses, partial payloads, injection via email subjects)
- LMDB transactions ↔ concurrent readers (map resize behavior, stale reader detection, transaction lifetime)
- UI thread ↔ search engine (result buffer ownership, cancellation during render, stale pointer access)
- Enterprise AD auth ↔ permission checking (token caching, impersonation scope, group membership freshness)
- Config loading ↔ runtime behavior (unclamped values causing OOB, integer overflow in computed limits)

### Avoid these anti-patterns

- Phantom findings: Don't report a bug you can't trace to a specific code path. "This might be vulnerable" with no evidence is not a finding.
- Stale findings: Check git blame — if a suspicious pattern was recently changed, read the fix commit before reporting.
- Test-contradicted claims: If a test directly exercises the exact scenario you're claiming is broken, and the test passes, downgrade or reject.
- Severity inflation: A Low bug that requires attacker-controlled archive files with local filesystem access is not High just because it involves "security."
- Duplicate root causes: If the same missing validation appears in 3 plugins, that's one finding with 3 affected locations, not 3 findings.

---
## High-priority bug classes

### Memory safety (C/C++ specific)

- Heap buffer overflows in string processing (trigram extraction, path concatenation, content parsing)
- Stack buffer overflows in fixed-size buffers (path buffers, format strings, config value parsing)
- Use-after-free in plugin lifecycle (plugin unloaded while work queue items reference plugin memory)
- Double-free in error paths (especially cleanup after partial initialization)
- Integer overflow/underflow in size calculations (record counts, batch sizes, mmap offsets, bloom filter sizing)
- Unchecked malloc/calloc return values leading to null pointer dereference
- Format string vulnerabilities (user-controlled strings passed to printf-family functions)
- Off-by-one in buffer sizing (null terminator, wide-char conversions, path separator appending)

### Boundary & scope enforcement

- Archive path traversal (symlinks, ".." sequences, absolute paths, null bytes, drive specifiers on Windows)
- Repository/workspace root containment bypasses (junction points, case folding, UNC paths)
- Plugin sandbox escape (plugin writing outside designated areas, accessing host internals)
- Content index size limit bypass (crafted files that decompress to exceed max_content_index_bytes)
- Thread count / batch size limits not enforced after config reload

### Data integrity through the pipeline

- LMDB transaction isolation (reader seeing partial batch commit, stale snapshot after map resize)
- String interning correctness (hash collision handling, deduplication under concurrent writers)
- Trigram index consistency (orphaned trigram entries after record deletion, index rebuild completeness)
- Bloom filter accuracy (false positive rate validation, correct bit-setting for Unicode strings)
- Zstd compression/decompression round-trip correctness (corrupted compressed data handling, size validation)
- Cache key collisions in bloom filter cache (eviction correctness under concurrent access)

### Concurrency & state

- MPMC queue correctness (ABA problem, memory ordering on non-x86 platforms, queue full/empty edge cases)
- CancelToken propagation completeness (especially through plugin boundaries and libcurl callbacks)
- CRITICAL_SECTION / pthread_mutex deadlock potential (lock ordering violations across subsystems)
- TOCTOU in file operations (stat then open, permission check then read, especially in credential files)
- Race conditions in adaptive thread pool sizing (NTFS scanner thread count adjustment)
- Shared mutable state in singleton services under concurrent requests
- LMDB reader slot exhaustion under concurrent GUI + indexer access

### Authentication & credential security

- OAuth token file permission verification completeness (Windows ACL vs Linux modes vs macOS)
- Credential material in memory (token lifetime, secure_memzero coverage, swap file exposure)
- Enterprise AD authentication (token replay, impersonation scope, LogonUser handle leaks)
- Cloud sync auth (team_id validation, cross-tenant isolation, shared index access control)

### Protocol & integration correctness

- libcurl error handling completeness (SSL errors, redirect following, DNS resolution failure)
- cJSON parsing of malicious/malformed API responses (null fields, type confusion, deeply nested objects)
- SQLite3 browser database reading (locked database handling, schema version mismatch, injection via history data)
- libgit2 repository access (malicious repo objects, submodule recursion depth, large blob handling)
- libarchive extraction (zip bombs, recursive archives, malformed headers, enormous file counts)
- IFilter/COM interop (COM initialization per-thread, interface leak, timeout on hung filters)
- NTFS USN journal parsing (journal wrap handling, incomplete records, volume dismount during tail)

### Platform-specific correctness

- Wide-char / UTF-8 conversion correctness (surrogate pairs, invalid sequences, buffer sizing for wchar_t)
- Windows-only code paths not guarded by platform checks (registry plugin on Linux, NTFS scanner on macOS)
- Case sensitivity differences (Windows paths case-insensitive, Linux case-sensitive, index lookup mismatch)
- Path separator normalization (backslash vs forward slash, mixed paths, trailing separator handling)
- Maximum path length handling (MAX_PATH on Windows, long path opt-in, network share UNC paths)

### Degradation signaling

- Silent false-clean: any path where a failure (plugin crash, parse error, timeout, malloc failure) produces an empty result instead of an error marker
- Plugin init failure not preventing indexer from proceeding with partial plugin set without warning
- LMDB map full not signaled to user (silent data loss on write failure)
- Network/API failures in cloud plugins treated as "no data" instead of "error"

---
## Required subsystem coverage

Report findings or explicit "no critical findings" for each:

┌─────┬───────────────────────────────────────┬──────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│  #  │               Subsystem               │                                       Key files to examine                                                  │
├─────┼───────────────────────────────────────┼──────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ 1   │ CLI entry + argument parsing          │ src/app/anything.c (wmain, argument parsing, mode dispatch)                                                 │
├─────┼───────────────────────────────────────┼──────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ 2   │ LMDB database engine                  │ src/core/database.c (transactions, string interning, trigram indexing, compression, batch writes)           │
├─────┼───────────────────────────────────────┼──────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ 3   │ Search engine + query parsing         │ src/core/search.c (trigram lookup, bloom filter cache, boolean operators, result ranking)                   │
├─────┼───────────────────────────────────────┼──────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ 4   │ Utility functions + SIMD              │ src/core/util.c (hash functions, SIMD paths, path manipulation, secure memory ops)                         │
├─────┼───────────────────────────────────────┼──────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ 5   │ NTFS USN journal scanner              │ src/platform/ntfs.c (USN record parsing, FRN map, journal tailing, adaptive thread pool)                   │
├─────┼───────────────────────────────────────┼──────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ 6   │ Platform scanners                     │ src/platform/scanner_win.cpp, scanner_linux.c, scanner_macos.c, apfs.c, wsl.c                              │
├─────┼───────────────────────────────────────┼──────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ 7   │ Archive extraction + path validation  │ src/core/archive.c or relevant archive handling code (path traversal defense, symlink checks, zip bombs)   │
├─────┼───────────────────────────────────────┼──────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ 8   │ Plugin system + host interface        │ include/anything/plugin.h, plugin loading code, work queue handoff, cancel token propagation               │
├─────┼───────────────────────────────────────┼──────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ 9   │ Code + Git plugins                    │ plugins/code/code_plugin.c, plugins/code/git_plugin.c                                                      │
├─────┼───────────────────────────────────────┼──────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ 10  │ Cloud/email plugins + OAuth           │ plugins/data_sources/gmail_plugin.c, microsoft_mail_plugin.c, icloud_plugin.c                              │
├─────┼───────────────────────────────────────┼──────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ 11  │ Web archive plugin                    │ plugins/data_sources/web_archive_plugin.c (browser DB reading, HTML parsing, URL fetching, cache)           │
├─────┼───────────────────────────────────────┼──────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ 12  │ OCR + media plugins                   │ plugins/media/ocr_plugin.c (Tesseract integration, image handling)                                         │
├─────┼───────────────────────────────────────┼──────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ 13  │ System plugins                        │ plugins/system/duplicates_plugin.c, registry_plugin.c                                                      │
├─────┼───────────────────────────────────────┼──────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ 14  │ Configuration loading + validation    │ src/core/config.c, include/anything/config.h (value clamping, parse errors, default drift)                 │
├─────┼───────────────────────────────────────┼──────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ 15  │ ImGui UI + rendering                  │ src/ui/ui_imgui.cpp (result display, preview generation, search interaction, thread safety with engine)     │
├─────┼───────────────────────────────────────┼──────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ 16  │ Concurrency primitives                │ MPMC queue (anything.h), thread pool management, CancelToken, lock ordering across subsystems              │
├─────┼───────────────────────────────────────┼──────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ 17  │ Enterprise features                   │ include/anything/enterprise.h, enterprise implementation (AD auth, network shares, audit log, permissions) │
├─────┼───────────────────────────────────────┼──────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ 18  │ Cloud sync + shared index             │ include/anything/cloud.h, cloud.c (upload/download, team_id isolation, credential handling)                │
├─────┼───────────────────────────────────────┼──────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ 19  │ Content extraction pipeline           │ Office file parsing, IFilter/COM interop, PDF extraction, PST/EML parsing                                  │
├─────┼───────────────────────────────────────┼──────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ 20  │ Third-party dependencies              │ third_party/cJSON (known vulnerabilities), vendored libraries, libcurl/libarchive usage patterns           │
└─────┴───────────────────────────────────────┴──────────────────────────────────────────────────────────────────────────────────────────────────────────────┘

---
## Mandatory test-awareness workflow

Before finalizing any finding's severity or the overall score:

1. Search for test files covering the affected subsystem.
2. Read representative tests — especially any that appear to directly test the scenario you're reporting.
3. Assign a Coverage Status to each finding:
   - Uncovered — no test exercises this scenario
   - Covered-Insufficient — tests exist but don't cover the specific failure mode
   - Covered-Validated — tests directly verify the invariant; finding is about a gap in the coverage, not the behavior
   - Contradicted-by-Tests — a passing test directly exercises the exact scenario claimed as broken
4. If Contradicted-by-Tests: either reject the finding or explain precisely why the test is insufficient despite appearing to cover it.

---
## Output format

### 1) Audit methodology

- Files inspected (with purpose)
- Test inventory summary (count by subsystem)
- Execution modes evaluated
- Test files read before conclusions (exact paths)

### 2) Findings by subsystem (all severities)

For each finding:
- Severity + Confidence
- Coverage Status
- Affected mode(s): CLI indexer / GUI search / plugin indexing / enterprise / cloud sync
- Why this is a real bug: the invariant that is violated
- Realistic impact: what goes wrong for an operator
- Trigger/minimal repro idea: concrete steps, not abstract scenarios
- Why tests might miss it: what the existing tests check vs what they don't
- Evidence: file:line citations

### 3) Top critical blockers (ranked)

### 4) Anything maturity rating (1–5)

┌─────────┬───────────────────────────────────────────────────────────────┐
│  Range  │                            Meaning                            │
├─────────┼───────────────────────────────────────────────────────────────┤
│ 1.0–2.0 │ Demo-grade, unsafe for routine desktop use                   │
├─────────┼───────────────────────────────────────────────────────────────┤
│ 2.1–3.0 │ Prototype, works on happy paths only                         │
├─────────┼───────────────────────────────────────────────────────────────┤
│ 3.1–4.0 │ Developer-grade, usable with explicit caveats                │
├─────────┼───────────────────────────────────────────────────────────────┤
│ 4.1–4.5 │ Production-grade, reliable under normal desktop workloads    │
├─────────┼───────────────────────────────────────────────────────────────┤
│ 4.6–5.0 │ Enterprise-grade, high trust under stress and edge conditions│
└─────────┴───────────────────────────────────────────────────────────────┘

Score must be justified by evidence, not feature count. Memory safety, correctness, and operator trust dominate.

### 5) Score movement rule

For each 0.2+ score increment toward the next tier, state:
- The specific fix required (not vague "improve X")
- The regression tests that would validate it
- Why this fix moves the needle (which invariant it restores)

### 6) Retest plan after fixes

### 7) Findings rejected after test review

For each rejected finding:
- Initially suspected issue
- Contradicting test evidence (file:line)
- Final disposition

---
## Rigor safeguards

- Executable code and test behavior are the source of truth — never comments or docs alone.
- Do not inflate score for feature breadth. A system with 100 features and 1 critical memory safety bug scores lower than a system with 10 features and 0.
- Explicitly flag uncertainty when a code path cannot be fully traced.
- One finding per root cause. List all affected locations under that finding.
- Do not report findings that were fixed in recent commits without checking the fix is complete.
- For C/C++ code: treat every unchecked buffer operation, missing bounds check, and unchecked allocation as a potential finding — but verify exploitability before assigning severity.
