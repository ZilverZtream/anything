#pragma once

#ifndef CLOUD_H
#define CLOUD_H
#include "anything/anything.h"
#include "anything/database.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CLOUD_ONEDRIVE,
    CLOUD_GOOGLE_DRIVE,
    CLOUD_PCLOUD,
    CLOUD_DROPBOX
} CloudProvider;

typedef struct SharedIndex {
    uint64_t team_id;
    uint8_t access_permissions;
    char shared_secret[32];
} SharedIndex;

BOOL CloudScanner_Start(CloudProvider provider, Db* db, MPMCQueue* out_queue);
BOOL CloudSync_CreateSharedIndex(SharedIndex* idx, uint64_t team_id, uint8_t permissions);
BOOL CloudSync_Upload(Db* db, CloudProvider provider, const SharedIndex* idx);
BOOL CloudSync_Download(Db* db, CloudProvider provider, const SharedIndex* idx);

#ifdef __cplusplus
}
#endif
#endif
