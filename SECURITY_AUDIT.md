# Security Audit - Credential Storage TODOs

## Status: DOCUMENTED - REQUIRES FUTURE IMPLEMENTATION

### Overview
The data source plugins (Microsoft Mail, Gmail, iCloud, Web Archive) currently store OAuth tokens and credentials using a basic file-based approach. This is **documented with security warnings** but requires proper OS credential manager integration for production use.

### Current Implementation

#### What Works ✅
1. **Linux**: Enforces 0600 file permissions (user-only read/write)
2. **All Platforms**: Warns users about insecure environment variable usage
3. **File-based**: Stores tokens in `~/.anything/` directory with restricted access
4. **Error handling**: Fails safely if permissions are incorrect (Linux)

#### What's Missing ❌
1. **Windows ACL Verification**: Cannot verify file permissions on Windows
   - Location: `plugins/data_sources/microsoft_mail_plugin.c:147`
   - Impact: Windows users may have insecure token files
   - Workaround: User is warned to manually verify permissions

2. **OS Credential Manager Integration**: Tokens should use platform APIs
   - **Windows**: Should use `CredWrite/CredRead` from Credential Manager
   - **macOS**: Should use Keychain Services API
   - **Linux**: Should use libsecret/gnome-keyring

### TODOs by Plugin

| Plugin | File | Line | Issue |
|--------|------|------|-------|
| Microsoft Mail | `microsoft_mail_plugin.c` | 115 | Missing OS credential manager |
| Microsoft Mail | `microsoft_mail_plugin.c` | 147 | Missing Windows ACL check |
| Gmail | `gmail_plugin.c` | 170 | Missing OS credential manager |
| iCloud | `icloud_plugin.c` | 146 | Missing OS credential manager |
| Web Archive | `web_archive_plugin.c` | 238 | Should implement local cache |

### Recommendation for Production

**ACCEPTABLE FOR NOW** because:
- User is warned multiple times about security implications
- Linux has proper permission enforcement (0600)
- Tokens are stored in user's home directory, not system-wide
- Plugins are opt-in data sources, not core functionality

**MUST FIX BEFORE 1.0** because:
- Windows has no permission verification
- Environment variables are inherently insecure
- Professional software should use OS credential managers

### Implementation Priority

1. **HIGH**: Implement Windows ACL checking (GetSecurityInfo)
2. **MEDIUM**: Add OS credential manager support as optional fallback
3. **LOW**: Keep file-based method with warnings for power users

---

**Audit Date**: 2025-12-04
**Auditor**: Production code review
**Status**: Documented, not blocking release with proper warnings
