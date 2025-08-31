// wsl.c - robust WSL filesystem scanner that reads ext4.vhdx directly
#include "wsl.h"
#include "util.h"

#include <windows.h>
#include <winreg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <ext2fs/ext2fs.h>
#include <ext2fs/ext2_io.h>

#include <sys/stat.h>

#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif

#define WSL_VHDX_OFFSET (1ULL<<20) // 1MB header before ext4 filesystem

// ---- Registry helper -------------------------------------------------
// Locate ext4.vhdx backing file for a given distro name
static BOOL find_vhdx_for_distro(const wchar_t* distro, wchar_t* out, size_t cch){
    HKEY hKey;
    if(RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss",
        0, KEY_READ, &hKey) != ERROR_SUCCESS) return FALSE;

    DWORD idx = 0; BOOL found = FALSE;
    wchar_t sub[256]; DWORD subLen;
    while(!found){
        subLen = (DWORD)ARRAYSIZE(sub);
        if(RegEnumKeyExW(hKey, idx++, sub, &subLen, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        HKEY hSub;
        if(RegOpenKeyExW(hKey, sub, 0, KEY_READ, &hSub) != ERROR_SUCCESS) continue;
        wchar_t name[256]; DWORD type = 0, bytes = sizeof(name);
        if(RegQueryValueExW(hSub, L"DistributionName", NULL, &type,
            (LPBYTE)name, &bytes) == ERROR_SUCCESS && type == REG_SZ){
            if(_wcsicmp(name, distro) == 0){
                wchar_t base[MAX_LONG_PATH]; bytes = sizeof(base);
                if(RegQueryValueExW(hSub, L"BasePath", NULL, &type,
                    (LPBYTE)base, &bytes) == ERROR_SUCCESS && type == REG_SZ){
                    _snwprintf(out, cch, L"%s\\ext4.vhdx", base);
                    found = TRUE;
                }
            }
        }
        RegCloseKey(hSub);
    }
    RegCloseKey(hKey);
    return found;
}

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

// ---- Minimal VHDX io_manager for libext2fs ---------------------------

typedef struct {
    HANDLE hFile;
} vhdx_priv;

static errcode_t vhdx_open(const char* name, int flags, io_channel* out){
    io_channel channel = (io_channel)calloc(1, sizeof(struct struct_io_channel));
    if(!channel) return ENOMEM;
    channel->magic   = EXT2_ET_MAGIC_IO_CHANNEL;
    channel->manager = &vhdx_manager;
    channel->block_size = 1024;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, name, -1, NULL, 0);
    wchar_t wname[MAX_LONG_PATH];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, wlen);
    HANDLE h = CreateFileW(wname, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if(h == INVALID_HANDLE_VALUE){ free(channel); return errno; }
    vhdx_priv* p = (vhdx_priv*)calloc(1,sizeof(vhdx_priv));
    if(!p){ CloseHandle(h); free(channel); return ENOMEM; }
    p->hFile = h; channel->private_data = p;
    channel->name = strdup(name);
    *out = channel;
    return 0;
}

static errcode_t vhdx_close(io_channel channel){
    if(!channel) return 0;
    vhdx_priv* p = (vhdx_priv*)channel->private_data;
    if(p){ if(p->hFile) CloseHandle(p->hFile); free(p); }
    free(channel->name);
    free(channel);
    return 0;
}

static errcode_t vhdx_set_blksize(io_channel channel, int blksize){
    channel->block_size = blksize;
    return 0;
}

static errcode_t vhdx_read_blk64(io_channel channel, unsigned long long block,
                                 int count, void* data){
    vhdx_priv* p = (vhdx_priv*)channel->private_data;
    LARGE_INTEGER off;
    off.QuadPart = WSL_VHDX_OFFSET + (LONGLONG)block * channel->block_size;
    if(!SetFilePointerEx(p->hFile, off, NULL, FILE_BEGIN)) return errno;
    DWORD to_read = count * channel->block_size;
    DWORD got = 0;
    if(!ReadFile(p->hFile, data, to_read, &got, NULL)) return errno;
    return (got == to_read) ? 0 : EIO;
}

static errcode_t vhdx_read_blk(io_channel channel, unsigned long block,
                               int count, void* data){
    return vhdx_read_blk64(channel, block, count, data);
}

static struct struct_io_manager vhdx_manager = {
    EXT2_ET_MAGIC_IO_MANAGER,
    "vhdx",
    vhdx_open,
    vhdx_close,
    vhdx_set_blksize,
    vhdx_read_blk,
    0, // write_blk
    0, // flush
    0, // write_byte
    0, // set_option
    0, // get_stats
    vhdx_read_blk64,
    0, // write_blk64
    0, // discard
    0, // cache_readahead
    0, // zeroout
    {0}
};

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

static int dir_iter_cb(ext2_ino_t dir, int entry, struct ext2_dir_entry* de,
                       int offset, int blocksize, char* buf, void* priv){
    DirIterCtx* ictx = (DirIterCtx*)priv;
    if(!de->inode) return 0;
    char name[256];
    int nlen = de->name_len;
    memcpy(name, de->name, nlen);
    name[nlen] = 0;
    if(strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;

    wchar_t wname[256];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 256);

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
        path_join(child, MAX_LONG_PATH, ictx->parent, wname);
        scan_directory(ictx->ctx, de->inode, child);
    }
    return 0;
}

static BOOL scan_directory(ScanCtx* ctx, ext2_ino_t ino, const wchar_t* parent){
    DirIterCtx ictx = { ctx, parent };
    errcode_t err = ext2fs_dir_iterate2(ctx->fs, ino, 0, NULL, dir_iter_cb, &ictx);
    return err == 0;
}

// ---- Entry point -----------------------------------------------------

BOOL WSLScanner_Start(const wchar_t* root_path, Db* db){
    if(!root_path || !db) return FALSE;

    const wchar_t* distro = root_path;
    const wchar_t* slash = wcsrchr(root_path, L'\\');
    if(slash) distro = slash + 1;

    wchar_t vhdx[MAX_LONG_PATH];
    if(!find_vhdx_for_distro(distro, vhdx, MAX_LONG_PATH)) return FALSE;

    char vhdx_utf8[MAX_LONG_PATH*3];
    WideCharToMultiByte(CP_UTF8, 0, vhdx, -1, vhdx_utf8, sizeof(vhdx_utf8), NULL, NULL);

    ext2_filsys fs;
    errcode_t err = ext2fs_open2(vhdx_utf8, NULL, EXT2_FLAG_RDONLY, 0, 0,
                                 &vhdx_manager, &fs);
    if(err) return FALSE;

    if(!db_begin_write(db)){ ext2fs_close(fs); return FALSE; }

    ScanCtx ctx = { fs, db };
    BOOL ok = scan_directory(&ctx, EXT2_ROOT_INO, root_path);

    if(ok) db_commit_write(db); else db_abort_write(db);

    ext2fs_close(fs);
    return ok;
}

