#pragma once

#ifndef ENTERPRISE_H
#define ENTERPRISE_H

/*
 * Enterprise feature declarations. These are gated behind the
 * ENTERPRISE compile-time macro so that the standard build
 * remains unaffected.
 */

#ifdef __cplusplus
extern "C" {
#endif

#ifdef ENTERPRISE
/* Index network shares such as SMB paths. */
void enterprise_index_network(const char *share);

/*
 * Authenticate a user via Active Directory.
 * Returns an opaque session handle on success, NULL on failure.
 * The caller must close the session with enterprise_close_session().
 */
void* enterprise_ad_login(const char *user, const char *password);

/* Close a session obtained from enterprise_ad_login(). */
void enterprise_close_session(void* session);

/* Get the username associated with a session. Returns "" if session is NULL. */
const char* enterprise_session_user(void* session);

/*
 * Check whether the authenticated user has read access to a given path.
 * Uses the session's user token for AccessCheck (fail-closed: returns 0
 * if session is NULL or any API call fails).
 */
int enterprise_check_permission(void* session, const char *path);

/* Log search queries for auditing purposes. */
void enterprise_audit_log(const char *user, const char *query);

/* Placeholder for centralized deployment helpers. */
void enterprise_deploy_msi(void);

#else
/* Stub inline implementations when enterprise features are disabled. */
static inline void enterprise_index_network(const char *share){ (void)share; }
static inline void* enterprise_ad_login(const char *user, const char *password){ (void)user; (void)password; return (void*)(size_t)1; /* non-NULL = success stub */ }
static inline void enterprise_close_session(void* session){ (void)session; }
static inline const char* enterprise_session_user(void* session){ (void)session; return ""; }
static inline int enterprise_check_permission(void* session, const char *path){ (void)session; (void)path; return 1; }
static inline void enterprise_audit_log(const char *user, const char *query){ (void)user; (void)query; }
static inline void enterprise_deploy_msi(void){}
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTERPRISE_H */
