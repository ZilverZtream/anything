#ifndef APFS_H
#define APFS_H

#include "anything.h"

#ifdef __APPLE__
#include <stdint.h>
typedef void (*APFSSnapshotCB)(const char* name, void* ctx);

BOOL apfs_list_snapshots(const char* volume, APFSSnapshotCB cb, void* ctx);
BOOL apfs_cloneid(const char* path, uint64_t* out_id);
#else
typedef void (*APFSSnapshotCB)(const char* name, void* ctx);

static inline BOOL apfs_list_snapshots(const char* volume, APFSSnapshotCB cb, void* ctx){ (void)volume; (void)cb; (void)ctx; return FALSE; }
static inline BOOL apfs_cloneid(const char* path, uint64_t* out_id){ (void)path; (void)out_id; return FALSE; }
#endif

#endif
