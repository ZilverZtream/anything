# AUDIT PASS 8 — Protocol + Integration

## Focus
HTTP/TLS handling, JSON parsing, Office file parsing, EXIF/ID3 metadata parsing, URL construction.

## Findings

### P8-001 — No TLS certificate verification enforcement (MEDIUM)
- **File:** `src/services/cloud.c` (http_request function)
- **Description:** The cloud HTTP request function uses libcurl but does not explicitly set `CURLOPT_SSL_VERIFYPEER` and `CURLOPT_SSL_VERIFYHOST`. While libcurl defaults these to enabled, an explicit set would be more robust against build configurations that disable them.
- **Recommendation:** Add explicit `curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L)`.

### P8-002 — EXIF IFD pointer validation insufficient (MEDIUM)
- **File:** `src/services/metadata.c`
- **Description:** The EXIF parser reads IFD pointers from file data (`rd32(exif+4,be)`) and uses them as offsets without validating they point within the EXIF segment. A crafted JPEG could cause out-of-bounds reads.
- **Recommendation:** Validate IFD offset < len before dereferencing.

### P8-003 — Office parser XML entity expansion (LOW)
- **File:** `src/services/office_parser.c`
- **Description:** The native Office parser extracts text from XML within OOXML files. While it uses a simple character-by-character parser (not an XML library), it doesn't explicitly reject DTD entity definitions. However, since it's not using an XML parser, entity expansion attacks (XXE) don't apply.
- **Status:** Low risk — custom parser ignores entities.

### P8-004 — Cloud strcpy for static strings (LOW)
- **File:** `src/services/cloud.c:325, 395, 483`
- **Description:** Several `strcpy` calls are used for constant-length static strings. While safe (destination buffers are large enough), they trigger compiler warnings.
- **Recommendation:** Replace with `strcpy_s` or `strncpy`.

### P8-005 — cJSON depth limit (LOW)
- **File:** `third_party/cJSON/cJSON.c`
- **Description:** cJSON has a default recursion depth limit of 1000. Cloud API responses should never reach this depth, but a malicious server could craft deeply nested JSON to cause stack overflow.
- **Status:** Low risk — depth limit exists in cJSON.

## Summary
| Severity | Count |
|----------|-------|
| Critical | 0 |
| High     | 0 |
| Medium   | 2 |
| Low      | 3 |
| Total    | 5 |
