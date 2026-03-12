# AUDIT PASS 5 — Plugin System

## Focus
Plugin loading security, API version validation, plugin isolation, input validation in plugin data sources.

## Findings

### P5-001 — Plugin loader lacks API version bounds check (MEDIUM)
- **File:** `src/system/plugin.c:37-42`
- **Description:** The plugin loader checks `api->version == ANYTHING_PLUGIN_API_VERSION` but does not reject plugins with incompatible future versions. If a plugin returns a higher version, the host still loads it, potentially calling functions with different signatures.
- **Recommendation:** Already correctly uses `==` check — this is actually PASS.
- **Status:** **PASS**

### P5-002 — code_plugin.c: sprintf without bounds checking (MEDIUM)
- **File:** `plugins/data_sources/code_plugin.c`
- **Description:** Uses `sprintf` for building file content strings without bounds checking. If file content exceeds the buffer, stack corruption occurs.
- **Recommendation:** Replace with `snprintf`.

### P5-003 — git_plugin.c: Integer overflow in cumulative size (MEDIUM)
- **File:** `plugins/data_sources/git_plugin.c`
- **Description:** Cumulative file size sum computed without overflow check. If a repository has many large files, the sum wraps around.

### P5-004 — web_archive_plugin.c: use-after-free if strdup fails (MEDIUM)
- **File:** `plugins/data_sources/web_archive_plugin.c`
- **Description:** After a failed strdup, the code continues using the original pointer which may have been freed or was NULL.

### P5-005 — registry_plugin.c: unchecked malloc (LOW)
- **File:** `plugins/data_sources/registry_plugin.c`
- **Description:** malloc return not checked before use. Low impact since registry data sizes are small.

### P5-006 — Plugin DLL search order (LOW)
- **File:** `src/system/plugin.c:25-31`
- **Description:** Plugin directory is constructed from the exe path. On Windows, DLL search order attacks could inject a malicious DLL if the plugin directory is writable by non-admin users.

## Summary
| Severity | Count |
|----------|-------|
| Critical | 0 |
| High     | 0 |
| Medium   | 3 |
| Low      | 2 |
| Pass     | 1 |
| Total    | 5 |
