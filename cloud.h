#ifndef CLOUD_H
#define CLOUD_H
#include "anything.h"
#include "database.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CLOUD_ONEDRIVE,
    CLOUD_GOOGLE_DRIVE,
    CLOUD_PCLOUD,
    CLOUD_DROPBOX
} CloudProvider;
BOOL CloudScanner_Start(CloudProvider provider, Db* db, MPMCQueue* out_queue);

#ifdef __cplusplus
}
#endif
#endif
