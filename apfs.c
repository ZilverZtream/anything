#include "apfs.h"
#ifdef __APPLE__
#include <sys/snapshot.h>
#include <sys/attr.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

BOOL apfs_list_snapshots(const char* volume, APFSSnapshotCB cb, void* ctx){
    if(!volume || !cb) return FALSE;
    int fd = open(volume, O_RDONLY);
    if(fd < 0) return FALSE;
    size_t bufsize = 4096;
    char* buf = (char*)malloc(bufsize);
    if(!buf){ close(fd); return FALSE; }
    while(fs_snapshot_list(fd, buf, (int)bufsize, 0) == 0){
        for(char* p = buf; *p; p += strlen(p)+1){
            cb(p, ctx);
        }
        if(buf[0]==0) break;
    }
    free(buf);
    close(fd);
    return TRUE;
}

BOOL apfs_cloneid(const char* path, uint64_t* out_id){
    if(!path || !out_id) return FALSE;
#ifdef ATTR_CMNEXT_CLONEID
    struct attrlist al; memset(&al,0,sizeof(al));
    al.bitmapcount = ATTR_BIT_MAP_COUNT;
    al.commonattr = ATTR_CMN_RETURNED_ATTRS | ATTR_CMNEXT_CLONEID;
    struct {
        uint32_t attr_ret;
        uint64_t cloneid;
    } buf;
    if(getattrlist(path, &al, &buf, sizeof(buf), FSOPT_ATTR_CMN_EXTENDED)==0){
        if(buf.attr_ret & ATTR_CMNEXT_CLONEID){
            *out_id = buf.cloneid;
            return TRUE;
        }
    }
#endif
    return FALSE;
}
#endif
