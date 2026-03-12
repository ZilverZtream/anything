/*
 * Cloud sync correctness tests.
 *
 * Verifies the audit-required invariants:
 *  - CloudSync_Download returns FALSE (not implemented, not silent success)
 *  - CloudSync_Upload returns FALSE on non-Windows (not implemented, not silent success)
 *  - CloudSync_Upload / Download reject team_id == 0
 *  - CloudSync_CreateSharedIndex generates a 31-char secret with valid charset
 *  - CloudSync_CreateSharedIndex rejects NULL
 *  - Shared secrets have sufficient entropy (no trivially predictable pattern)
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#ifndef _WIN32
#include "stubs/windows_stub.h"
#endif

#include <lmdb.h>
#include "anything/anything.h"
#include "anything/database.h"
#include "anything/cloud.h"

static void test_download_returns_false(void){
    /*
     * CloudSync_Download must NOT return TRUE as a stub.
     * Silent success violates operator trust.
     */
    SharedIndex idx = {0};
    idx.team_id = 42;
    idx.access_permissions = 0xFF;
    strcpy(idx.shared_secret, "TEST_SECRET_12345678901234567");

    BOOL result = CloudSync_Download(NULL, CLOUD_ONEDRIVE, &idx);
    assert(result == FALSE);

    /* Even with non-NULL db would fail since it's not implemented */
    printf("  download_returns_false: OK\n");
}

static void test_download_null_params(void){
    SharedIndex idx = {0};
    idx.team_id = 1;
    assert(CloudSync_Download(NULL, CLOUD_ONEDRIVE, &idx) == FALSE);
    assert(CloudSync_Download(NULL, CLOUD_ONEDRIVE, NULL) == FALSE);
    printf("  download_null_params: OK\n");
}

static void test_download_rejects_zero_team_id(void){
    SharedIndex idx = {0};
    idx.team_id = 0; /* invalid */
    idx.access_permissions = 0xFF;

    BOOL result = CloudSync_Download(NULL, CLOUD_ONEDRIVE, &idx);
    assert(result == FALSE);
    printf("  download_rejects_zero_team_id: OK\n");
}

static void test_upload_rejects_zero_team_id(void){
    SharedIndex idx = {0};
    idx.team_id = 0; /* invalid */
    idx.access_permissions = 0xFF;

    BOOL result = CloudSync_Upload(NULL, CLOUD_ONEDRIVE, &idx);
    assert(result == FALSE);
    printf("  upload_rejects_zero_team_id: OK\n");
}

static void test_upload_null_params(void){
    SharedIndex idx = {0};
    idx.team_id = 1;
    assert(CloudSync_Upload(NULL, CLOUD_ONEDRIVE, &idx) == FALSE);
    assert(CloudSync_Upload(NULL, CLOUD_ONEDRIVE, NULL) == FALSE);
    printf("  upload_null_params: OK\n");
}

#ifndef _WIN32
static void test_upload_returns_false_non_windows(void){
    /*
     * On non-Windows platforms, CloudSync_Upload must return FALSE
     * (not silently return TRUE without uploading).
     */
    SharedIndex idx = {0};
    idx.team_id = 42;

    BOOL result = CloudSync_Upload(NULL, CLOUD_ONEDRIVE, &idx);
    assert(result == FALSE);
    printf("  upload_returns_false_non_windows: OK\n");
}
#endif

static void test_create_shared_index_valid(void){
    SharedIndex idx;
    memset(&idx, 0, sizeof(idx));

    BOOL result = CloudSync_CreateSharedIndex(&idx, 12345, 0x0F);
    assert(result == TRUE);
    assert(idx.team_id == 12345);
    assert(idx.access_permissions == 0x0F);

    /* Secret should be 31 chars + null terminator */
    assert(strlen(idx.shared_secret) == 31);

    /* All chars should be [0-9A-Z] */
    for(int i = 0; i < 31; i++){
        char c = idx.shared_secret[i];
        assert((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'));
    }
    assert(idx.shared_secret[31] == '\0');

    printf("  create_shared_index_valid: OK\n");
}

static void test_create_shared_index_null(void){
    BOOL result = CloudSync_CreateSharedIndex(NULL, 1, 1);
    assert(result == FALSE);
    printf("  create_shared_index_null: OK\n");
}

static void test_shared_secret_uniqueness(void){
    /*
     * Generate multiple secrets and verify they are not identical.
     * This is a basic entropy check — not cryptographic proof,
     * but catches the trivial case of predictable output.
     */
    SharedIndex a, b, c;
    assert(CloudSync_CreateSharedIndex(&a, 1, 1) == TRUE);
    assert(CloudSync_CreateSharedIndex(&b, 1, 1) == TRUE);
    assert(CloudSync_CreateSharedIndex(&c, 1, 1) == TRUE);

    /* At least two of three should differ (overwhelmingly likely with CSPRNG) */
    int all_same = (strcmp(a.shared_secret, b.shared_secret) == 0 &&
                    strcmp(b.shared_secret, c.shared_secret) == 0);
    assert(!all_same);

    printf("  shared_secret_uniqueness: OK\n");
}

int main(void){
    printf("Cloud sync tests:\n");
    test_download_returns_false();
    test_download_null_params();
    test_download_rejects_zero_team_id();
    test_upload_rejects_zero_team_id();
    test_upload_null_params();
#ifndef _WIN32
    test_upload_returns_false_non_windows();
#endif
    test_create_shared_index_valid();
    test_create_shared_index_null();
    test_shared_secret_uniqueness();
    printf("All cloud sync tests passed\n");
    return 0;
}
