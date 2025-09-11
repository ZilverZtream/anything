// macvm.c - scanner for macOS virtualization disk images (Docker, Parallels, VMware)
#include "macvm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

#include <ext2fs/ext2fs.h>
#include <ext2fs/ext2_io.h>
#ifdef HAS_LIBVMDK
#include <libvmdk.h>
#endif


#ifndef FILE_ATTRIBUTE_DIRECTORY
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#endif
#ifndef FILE_ATTRIBUTE_READONLY
#define FILE_ATTRIBUTE_READONLY 0x1
#endif
#ifndef FILE_ATTRIBUTE_NORMAL
#define FILE_ATTRIBUTE_NORMAL 0x80
#endif
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif

// ---- Time/attribute helpers ------------------------------------------
static uint64_t unix_time_to_filetime(uint32_t t){
    return ((uint64_t)t + 11644473600ULL) * 10000000ULL;
}

static uint32_t ext_mode_to_attrs(uint16_t mode){
    uint32_t a = 0;
    if(S_ISDIR(mode)) a |= FILE_ATTRIBUTE_DIRECTORY;
    if(!(mode & 0200)) a |= FILE_ATTRIBUTE_READONLY;
    if(!a) a = FILE_ATTRIBUTE_NORMAL;
    return a;
}

#ifdef HAS_LIBVMDK

// ---- Minimal VMDK io_manager for libext2fs ---------------------------
typedef struct {
    libvmdk_handle_t* handle;
} vmdk_priv;

static errcode_t vmdk_open(const char* name, int flags, io_channel* out){
    (void)flags;
    io_channel channel = (io_channel)calloc(1, sizeof(struct struct_io_channel));
    if(!channel) return ENOMEM;
    channel->magic = EXT2_ET_MAGIC_IO_CHANNEL;
    channel->manager = &vmdk_manager;
    channel->block_size = 1024;

    libvmdk_error_t* error = NULL;
    libvmdk_handle_t* handle = NULL;
    if(libvmdk_handle_initialize(&handle, &error) != 1) goto fail;
    int access = libvmdk_get_access_flags_read();
    if(libvmdk_handle_open(handle, name, access, &error) != 1) goto fail;
    if(libvmdk_handle_open_extent_data_files(handle, &error) != 1) goto fail;

    vmdk_priv* p = (vmdk_priv*)calloc(1, sizeof(vmdk_priv));
    if(!p) goto fail;
    p->handle = handle;
    channel->private_data = p;
    channel->name = strdup(name);
    *out = channel;
    if(error) libvmdk_error_free(&error);
    return 0;
fail:
    if(error) libvmdk_error_free(&error);
    if(handle){ libvmdk_handle_close(handle, NULL); libvmdk_handle_free(&handle, NULL); }
    free(channel);
    return EIO;
}

static errcode_t vmdk_close(io_channel channel){
    if(!channel) return 0;
    vmdk_priv* p = (vmdk_priv*)channel->private_data;
    if(p){
        if(p->handle){
            libvmdk_handle_close(p->handle, NULL);
            libvmdk_handle_free(&p->handle, NULL);
        }
        free(p);
    }
    free(channel->name);
    free(channel);
    return 0;
}

static errcode_t vmdk_set_blksize(io_channel channel, int blksize){
    channel->block_size = blksize;
    return 0;
}

static errcode_t vmdk_read_blk64(io_channel channel, unsigned long long block,
                                 int count, void* data){
    vmdk_priv* p = (vmdk_priv*)channel->private_data;
    size_t to_read = (size_t)count * channel->block_size;
    ssize_t got = libvmdk_handle_read_buffer_at_offset(
        p->handle, data, to_read, (off64_t)block * channel->block_size, NULL);
    return (got == (ssize_t)to_read) ? 0 : EIO;
}

static errcode_t vmdk_read_blk(io_channel channel, unsigned long block,
                               int count, void* data){
    return vmdk_read_blk64(channel, block, count, data);
}

static struct struct_io_manager vmdk_manager = {
    EXT2_ET_MAGIC_IO_MANAGER,
    "vmdk",
    vmdk_open,
    vmdk_close,
    vmdk_set_blksize,
    vmdk_read_blk,
    0,
    0,
    0,
    0,
    0,
    vmdk_read_blk64,
    0,
    0,
    0,
    0,
    {0}
};

#endif // HAS_LIBVMDK

// ---- Directory traversal ---------------------------------------------
typedef struct {
    ext2_filsys fs;
    Db* db;
} ScanCtx;

static BOOL scan_directory(ScanCtx* ctx, ext2_ino_t ino, const wchar_t* parent);

typedef struct {
    ScanCtx* ctx;
    const wchar_t* parent;
} DirIterCtx;

static BOOL path_join_local(wchar_t* dst, size_t dstcch, const wchar_t* a, const wchar_t* b){
    if(!dst || !a || !b) return FALSE;
    wcscpy(dst, a);
    size_t n = wcslen(dst);
    if(n>0 && dst[n-1]!=L'/'){
        if(n+1>=dstcch) return FALSE;
        dst[n++] = L'/';
        dst[n] = 0;
    }
    if(wcslen(b)+n>=dstcch) return FALSE;
    wcscat_s(dst, dstcch, b);
    return TRUE;
}

static int dir_iter_cb(ext2_ino_t dir, int entry, struct ext2_dir_entry* de,
                       int offset, int blocksize, char* buf, void* priv){
    (void)dir; (void)entry; (void)offset; (void)blocksize; (void)buf;
    DirIterCtx* ictx = (DirIterCtx*)priv;
    if(!de->inode) return 0;
    char name[256];
    int nlen = de->name_len;
    memcpy(name, de->name, nlen);
    name[nlen] = 0;
    if(strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;

    wchar_t wname[256];
    mbstowcs(wname, name, 256);

    struct ext2_inode inode;
    if(ext2fs_read_inode(ictx->ctx->fs, de->inode, &inode)) return 0;

    DbRecord rec = {0};
    rec.type = S_ISDIR(inode.i_mode) ? DB_REC_DIR : DB_REC_FILE;
    rec.parent_str_id = db_intern_wstring(ictx->ctx->db, ictx->parent);
    rec.name_str_id   = db_intern_wstring(ictx->ctx->db, wname);
    rec.file_size = ((uint64_t)inode.i_size_high << 32) | inode.i_size;
    rec.creation_time = unix_time_to_filetime(inode.i_ctime);
    rec.modified_time = unix_time_to_filetime(inode.i_mtime);
    rec.access_time   = unix_time_to_filetime(inode.i_atime);
    rec.attributes    = ext_mode_to_attrs(inode.i_mode);
    db_put_records(ictx->ctx->db, &rec, 1);

    if(S_ISDIR(inode.i_mode)){
        wchar_t child[MAX_LONG_PATH];
        if(path_join_local(child, MAX_LONG_PATH, ictx->parent, wname))
            scan_directory(ictx->ctx, de->inode, child);
    }
    return 0;
}

static BOOL scan_directory(ScanCtx* ctx, ext2_ino_t ino, const wchar_t* parent){
    DirIterCtx ictx = { ctx, parent };
    errcode_t err = ext2fs_dir_iterate2(ctx->fs, ino, 0, NULL, dir_iter_cb, &ictx);
    return err == 0;
}

static BOOL ends_with(const char* s, const char* ext){
    size_t ls = strlen(s), le = strlen(ext);
    if(ls < le) return FALSE;
    return strcasecmp(s + ls - le, ext) == 0;
}

// ---- Entry point -----------------------------------------------------
BOOL MacVMScanner_Start(const wchar_t* image_path, Db* db){
    if(!image_path || !db) return FALSE;

    char img_utf8[MAX_LONG_PATH*3];
    wcstombs(img_utf8, image_path, sizeof(img_utf8));

    ext2_filsys fs;
    const struct struct_io_manager* iom = unix_io_manager;
#ifdef HAS_LIBVMDK
    if(ends_with(img_utf8, ".vmdk"))
        iom = &vmdk_manager;
#endif
    errcode_t err = ext2fs_open2(img_utf8, NULL, EXT2_FLAG_RDONLY, 0, 0,
                                 iom, &fs);
    if(err) return FALSE;

    if(!db_begin_write(db)){ ext2fs_close(fs); return FALSE; }

    ScanCtx ctx = { fs, db };
    BOOL ok = scan_directory(&ctx, EXT2_ROOT_INO, image_path);

    if(ok) db_commit_write(db); else db_abort_write(db);

    ext2fs_close(fs);
    return ok;
}
