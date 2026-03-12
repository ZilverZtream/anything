# AUDIT PASS 6 — Auth + Credentials

## Focus
OAuth token storage, DPAPI encryption, enterprise AD authentication, credential exposure, CSPRNG, sensitive memory handling.

## Findings

### P6-001 — OAuth tokens read from environment variables (MEDIUM)
- **Files:** `plugins/data_sources/gmail_plugin.c:173`, `icloud_plugin.c:149`, `microsoft_mail_plugin.c`
- **Description:** All mail plugins read tokens from environment variables (GMAIL_TOKEN, ICLOUD_TOKEN, etc.). Environment variables are readable by other processes on the same machine and may persist in process listings. Each plugin already prints a warning about this.
- **Status:** Already has warning comments and stderr output. TODO comments reference OS credential manager migration.

### P6-002 — Cloud token DPAPI encryption is sound (PASS)
- **File:** `src/services/cloud.c:29-93`
- **Description:** Uses VirtualAlloc+VirtualLock for sensitive buffers (preventing swap), SecureZeroMemory for clearing, DPAPI CryptProtectData for encryption. Comprehensive implementation.
- **Status:** **PASS** — well-implemented.

### P6-003 — CSPRNG implementation correct (PASS)
- **File:** `src/services/cloud.c:680-720`
- **Description:** Uses BCryptGenRandom on Windows, /dev/urandom on Linux, SecRandomCopyBytes on macOS. All are cryptographically secure sources.
- **Status:** **PASS**

### P6-004 — Enterprise session tokens not time-limited (MEDIUM)
- **File:** `src/enterprise/enterprise.c`
- **Description:** Session tokens are created on successful AD authentication but no expiry time is set. A captured session token remains valid indefinitely until the process restarts.
- **Recommendation:** Add timestamp-based expiry to EnterpriseSession.

### P6-005 — Stack-allocated token buffers not zeroed (LOW)
- **File:** `src/services/cloud.c:636`, `plugins/data_sources/*.c`
- **Description:** `char token_utf8[4096]` stack buffers contain plaintext tokens but are not zeroed before function return. A stack spray or crash dump could expose them.
- **Recommendation:** Add `secure_memzero(token_utf8, sizeof(token_utf8))` before return.

## Summary
| Severity | Count |
|----------|-------|
| Critical | 0 |
| High     | 0 |
| Medium   | 2 |
| Low      | 1 |
| Pass     | 2 |
| Total    | 3 |
