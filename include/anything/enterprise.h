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

/* Check whether a user has access to a given path. */
int enterprise_check_permission(const char *user, const char *path);

/* Log search queries for auditing purposes. */
void enterprise_audit_log(const char *user, const char *query);

/* Authenticate a user via Active Directory. */
int enterprise_ad_authenticate(const char *user, const char *password);

/* Placeholder for centralized deployment helpers. */
void enterprise_deploy_msi(void);

#else
/* Stub inline implementations when enterprise features are disabled. */
static inline void enterprise_index_network(const char *share){ (void)share; }
static inline int enterprise_check_permission(const char *user, const char *path){ (void)user; (void)path; return 1; }
static inline void enterprise_audit_log(const char *user, const char *query){ (void)user; (void)query; }
static inline int enterprise_ad_authenticate(const char *user, const char *password){ (void)user; (void)password; return 1; }
static inline void enterprise_deploy_msi(void){}
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTERPRISE_H */
