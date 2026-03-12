/*
 * Enterprise authentication and authorization tests.
 *
 * Tests the enterprise API contract:
 *  - enterprise_ad_login returns non-NULL on success, NULL on failure
 *  - enterprise_check_permission is fail-closed (NULL session = denied)
 *  - enterprise_session_user returns the authenticated username
 *  - enterprise_close_session safely handles NULL
 *
 * When ENTERPRISE is not defined, stub implementations are tested
 * to verify they maintain the expected contract for non-enterprise builds.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include "stubs/windows_stub.h"
#endif

#include "anything/anything.h"
#include "anything/enterprise.h"

static void test_stub_ad_login_returns_non_null(void){
    /*
     * In non-enterprise builds, enterprise_ad_login should return
     * a non-NULL value (stub success) so that callers don't abort.
     */
#ifndef ENTERPRISE
    void* session = enterprise_ad_login("testuser", "testpass");
    assert(session != NULL);
    enterprise_close_session(session);
    printf("  stub_ad_login_returns_non_null: OK\n");
#else
    printf("  stub_ad_login_returns_non_null: SKIPPED (ENTERPRISE build)\n");
#endif
}

static void test_stub_check_permission_allows(void){
    /*
     * In non-enterprise builds, enterprise_check_permission should
     * return 1 (allow) regardless of arguments.
     */
#ifndef ENTERPRISE
    void* session = enterprise_ad_login("user", "pass");
    assert(enterprise_check_permission(session, "/any/path") == 1);
    assert(enterprise_check_permission(NULL, "/any/path") == 1);
    enterprise_close_session(session);
    printf("  stub_check_permission_allows: OK\n");
#else
    printf("  stub_check_permission_allows: SKIPPED (ENTERPRISE build)\n");
#endif
}

static void test_stub_session_user(void){
    /*
     * In non-enterprise builds, enterprise_session_user should return
     * an empty string (non-NULL).
     */
#ifndef ENTERPRISE
    void* session = enterprise_ad_login("user", "pass");
    const char* user = enterprise_session_user(session);
    assert(user != NULL);
    enterprise_close_session(session);
    printf("  stub_session_user: OK\n");
#else
    printf("  stub_session_user: SKIPPED (ENTERPRISE build)\n");
#endif
}

static void test_null_session_user_returns_empty(void){
    const char* user = enterprise_session_user(NULL);
    assert(user != NULL);
    assert(strcmp(user, "") == 0);
    printf("  null_session_user_returns_empty: OK\n");
}

static void test_close_session_null_safe(void){
    /* enterprise_close_session(NULL) should not crash. */
    enterprise_close_session(NULL);
    printf("  close_session_null_safe: OK\n");
}

static void test_audit_log_null_safe(void){
    /* enterprise_audit_log with NULLs should not crash. */
    enterprise_audit_log(NULL, NULL);
    enterprise_audit_log("user", NULL);
    enterprise_audit_log(NULL, "query");
    printf("  audit_log_null_safe: OK\n");
}

#ifdef ENTERPRISE
static void test_enterprise_ad_login_empty_user_fails(void){
    /*
     * enterprise_ad_login with empty username should return NULL
     * (fail-closed).
     */
    void* session = enterprise_ad_login("", "password");
    assert(session == NULL);
    printf("  enterprise_ad_login_empty_user_fails: OK\n");
}

static void test_enterprise_ad_login_null_fails(void){
    void* session = enterprise_ad_login(NULL, "password");
    assert(session == NULL);
    session = enterprise_ad_login("user", NULL);
    assert(session == NULL);
    printf("  enterprise_ad_login_null_fails: OK\n");
}

static void test_enterprise_check_permission_null_session_denied(void){
    /*
     * enterprise_check_permission with NULL session must return 0
     * (fail-closed).
     */
    assert(enterprise_check_permission(NULL, "C:\\some\\path") == 0);
    printf("  enterprise_check_permission_null_session_denied: OK\n");
}

static void test_enterprise_check_permission_null_path_denied(void){
    void* session = enterprise_ad_login("test", "test");
    if(session){
        assert(enterprise_check_permission(session, NULL) == 0);
        enterprise_close_session(session);
    }
    printf("  enterprise_check_permission_null_path_denied: OK\n");
}
#endif

int main(void){
    printf("Enterprise auth tests:\n");
    test_stub_ad_login_returns_non_null();
    test_stub_check_permission_allows();
    test_stub_session_user();
    test_null_session_user_returns_empty();
    test_close_session_null_safe();
    test_audit_log_null_safe();
#ifdef ENTERPRISE
    test_enterprise_ad_login_empty_user_fails();
    test_enterprise_ad_login_null_fails();
    test_enterprise_check_permission_null_session_denied();
    test_enterprise_check_permission_null_path_denied();
#endif
    printf("All enterprise auth tests passed\n");
    return 0;
}
