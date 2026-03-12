# AUDIT PASS 2 — Archive + Path Traversal

## Focus
Archive extraction paths, directory traversal prevention, path manipulation across all subsystems including cloud drive scanners, generic/network scanners, WSL scanner, and the archive indexer.

## Findings

### P2-001 — Cloud API filenames not sanitized for path traversal (MEDIUM)
- **File:** `src/services/cloud.c:423-428, 513-518, 551-555`
- **Subsystem:** Cloud drive scanners (OneDrive, Google Drive, pCloud, Dropbox)
- **Description:** Cloud API responses provide file/folder names that are directly used in `path_join()` to build parent paths. A malicious or compromised cloud API could return a name containing `..` or absolute path components (e.g., `"name": "../../etc"`) which would escape the virtual root. While `path_join` uses `wcscpy_s`/`wcsncat` for bounds, it does not reject `..` segments.
- **Impact:** Index entries with traversal paths could be stored in the database, and if any export/extraction feature exists, could map to unexpected filesystem locations.
- **Recommendation:** Add a `name_safe()` validation that rejects names containing `..`, `/`, `\`, or `:` before passing to `path_join`.

### P2-002 — pCloud auth token in URL query parameter (MEDIUM)
- **File:** `src/services/cloud.c:564`
- **Subsystem:** Cloud
- **Description:** `snprintf(path,512,"/listfolder?auth=%s&folderid=0&recursive=1", token)` embeds the OAuth token directly in the URL query string. This token may appear in server logs, proxy caches, and browser history. All other cloud providers pass the token in the `Authorization` header.
- **Impact:** Token exposure via URL logging.
- **Recommendation:** Move pCloud token to a request header if the API supports it, or document the limitation.

### P2-003 — Google Drive API query string injection (LOW)
- **File:** `src/services/cloud.c:473`
- **Subsystem:** Cloud
- **Description:** `curr->id` (from server JSON response) is interpolated directly into a query string without URL-encoding: `snprintf(path,1024,"/drive/v3/files?q='%s'+in+parents&fields=...)", curr->id)`. A malicious id containing `'` or `&` could alter the query semantics.
- **Impact:** Could cause unexpected API behavior or retrieve unintended data.
- **Recommendation:** URL-encode the folder id before interpolation.

### P2-004 — exfat scanner: reparse points skipped but junctions not explicitly checked (LOW)
- **File:** `src/services/exfat.c:89`
- **Subsystem:** Generic scanner
- **Description:** The scanner correctly checks `FILE_ATTRIBUTE_REPARSE_POINT` before descending into subdirectories, preventing symlink/junction following. This is correct behavior — no fix needed.
- **Status:** **PASS** — no issue.

### P2-005 — Archive URL-decode could produce NUL bytes (LOW)
- **File:** `src/services/archive.c:50`
- **Subsystem:** Archive indexer
- **Description:** URL-decoding `%00` produces a NUL byte in `decoded[]`, which would truncate the path at that point. While this doesn't cause a traversal (the truncated path is still validated), it could cause some archive entries to silently map to unexpected shorter paths.
- **Impact:** Minimal — could cause duplicate/incorrect index entries but no security breach.
- **Recommendation:** Reject `%00` encoding explicitly.

### P2-006 — WSL scanner: ext2 directory names not bounds-checked (LOW)
- **File:** `src/platform/wsl.c:160-162`
- **Subsystem:** WSL
- **Description:** `de->name_len` is copied into a 256-byte buffer. ext2 name_len is guaranteed to be at most 255 by the filesystem format, and the buffer is 256 bytes, so this is safe. The `..` check at line 163 prevents traversal.
- **Status:** **PASS** — no issue.

## Summary
| Severity | Count |
|----------|-------|
| Critical | 0 |
| High     | 0 |
| Medium   | 2 |
| Low      | 2 |
| Pass     | 2 |

## Rejected Findings
- Archive normalization (archive.c): Already has 3-layer defense (pre-decode check, post-decode check, final strstr check). Very robust.
- exfat reparse point check: Already correct.
- WSL name bounds: Already safe due to ext2 invariants.
