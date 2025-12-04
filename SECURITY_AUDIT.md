# Security Audit - Credential Storage TODOs

## Status: DOCUMENTED - REQUIRES FUTURE IMPLEMENTATION

### Overview
The data source plugins (Microsoft Mail, Gmail, iCloud, Web Archive) currently store OAuth tokens and credentials using a basic file-based approach. This is **documented with security warnings** but requires proper OS credential manager integration for production use.

### Current Implementation

#### What Works ✅
1. **Linux**: Enforces 0600 file permissions (user-only read/write)
2. **Windows**: Verifies file ACLs using GetSecurityInfo API to ensure owner-only access
3. **All Platforms**: Warns users about insecure environment variable usage
4. **File-based**: Stores tokens in `~/.anything/` directory with restricted access
5. **Error handling**: Fails safely if permissions are incorrect (all platforms)
6. **Windows**: Sets owner-only ACLs when creating token store files

#### What's Missing ❌
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

1. **~~HIGH~~** ✅ **COMPLETED**: Implement Windows ACL checking (GetSecurityInfo)
   - Implemented in `microsoft_mail_plugin.c` with `verify_windows_acl()` function
   - Verifies file ownership and checks for unauthorized access by other users/groups
   - Sets owner-only ACLs when creating token store files
2. **MEDIUM**: Add OS credential manager support as optional fallback
3. **LOW**: Keep file-based method with warnings for power users

---

**Audit Date**: 2025-12-04
**Last Update**: 2025-12-04
**Auditor**: Production code review
**Status**: Windows ACL checking implemented; remaining TODOs documented and not blocking release
