# Security Audit - Credential Storage TODOs

## Status: DOCUMENTED - REQUIRES FUTURE IMPLEMENTATION

### Overview
The data source plugins (Microsoft Mail, Gmail, iCloud, Web Archive) currently store OAuth tokens and credentials using a basic file-based approach. This is **documented with security warnings** but requires proper OS credential manager integration for production use.

### Current Implementation

#### What Works
1. **Linux**: Enforces 0600 file permissions (user-only read/write)
2. **Windows**: Verifies file ACLs using GetSecurityInfo API to ensure owner-only access
3. **All Platforms**: Warns users about insecure environment variable usage
4. **File-based**: Stores tokens in `~/.anything/` directory with restricted access
5. **Error handling**: Fails safely if permissions are incorrect (all platforms)
6. **Windows**: Sets owner-only ACLs when creating token store files

#### What's Missing
1. **OS Credential Manager Integration**: Tokens should use platform APIs
   - **Windows**: Should use `CredWrite/CredRead` from Credential Manager
   - **macOS**: Should use Keychain Services API
   - **Linux**: Should use libsecret/gnome-keyring

### TODOs by Plugin

| Plugin | File | Line | Issue | Status |
|--------|------|------|-------|--------|
| Microsoft Mail | `microsoft_mail_plugin.c` | ~224 | Missing OS credential manager | TODO |
| Gmail | `gmail_plugin.c` | 170 | Missing OS credential manager | TODO |
| iCloud | `icloud_plugin.c` | 146 | Missing OS credential manager | TODO |
| Web Archive | `web_archive_plugin.c` | 238 | Should implement local cache | TODO |

### Recommendation for Production

**ACCEPTABLE FOR NOW** because:
- User is warned multiple times about security implications
- Linux has proper permission enforcement (0600)
- Windows has proper ACL verification and enforcement
- Tokens are stored in user's home directory, not system-wide
- Plugins are opt-in data sources, not core functionality

**SHOULD FIX FOR 1.0** because:
- Environment variables are inherently insecure
- Professional software should use OS credential managers

### Implementation Priority

1. **~~HIGH~~** COMPLETED: Implement Windows ACL checking (GetSecurityInfo)
   - Implemented in `microsoft_mail_plugin.c` with `verify_windows_acl()` function
   - Verifies file ownership and checks for unauthorized access by other users/groups
   - Sets owner-only ACLs when creating token store files
2. **MEDIUM**: Add OS credential manager support as optional fallback
3. **LOW**: Keep file-based method with warnings for power users

---

## Audit Fix Log (Section 5/6 Remediation)

### Fixes Applied

| Finding | Severity | Fix | Files Changed |
|---------|----------|-----|---------------|
| Plugin loader OOB write | High | Pre-check bounds before array store in Windows do...while loop | `src/system/plugin.c` |
| Enterprise auth bypass | High | New session-based API (`enterprise_ad_login`/`enterprise_check_permission` with user token), fail-closed, no hardcoded credentials | `include/anything/enterprise.h`, `src/enterprise/enterprise.c`, `src/core/search.c`, `src/app/anything.c` |
| Cloud sync false-success | High | `CloudSync_Download`/`CloudSync_Upload` (non-Windows) now return FALSE instead of silent TRUE; team_id=0 rejected | `src/services/cloud.c` |
| Predictable shared secret | Medium | Replaced `rand()%36` with CSPRNG (`RtlGenRandom`/`/dev/urandom`) | `src/services/cloud.c` |
| Queue stall on cancel | Medium | Added `CancelToken` and max-retry bounds to `enqueue_item` loop | `src/services/cloud.c`, `include/anything/cloud.h` |

### Tests Added

| Test File | Covers |
|-----------|--------|
| `tests/plugin_test.c` | Plugin loader bounds enforcement (>16 plugins) |
| `tests/enterprise_test.c` | Enterprise auth API contract (fail-closed, NULL safety) |
| `tests/cloud_test.c` | Cloud sync false-success prevention, team_id validation, secret entropy |
| `tests/queue_test.c` | MPMC queue operations, cancel token stops blocked producer |

---

**Audit Date**: 2025-12-04
**Last Update**: 2026-03-12
**Auditor**: Production code review
**Status**: Section 5/6 audit remediation complete; credential storage TODOs remain documented
