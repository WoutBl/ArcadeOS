/*
 * ArcadeOS – Virtual File System (VFS)
 *
 * "Everything is a file." Every file, directory, and device is represented
 * by a vfs_node_t containing function pointers for its operations.
 */

#ifndef VFS_H
#define VFS_H

#include "types.h"

/* ──────── Node type flags ──────── */
#define VFS_FLAG_FILE    0x01   /* Regular file */
#define VFS_FLAG_DIR     0x02   /* Directory */
#define VFS_FLAG_DEVICE  0x04   /* Character device (e.g. /dev/tty) */
#define VFS_FLAG_MOUNT   0x08   /* Mount point */

/* ──────── File Descriptor constants ──────── */
#define MAX_FD      16   /* Max open files per process */
#define STDIN_FD     0
#define STDOUT_FD    1
#define STDERR_FD    2

/* ──────── Forward declaration ──────── */
struct vfs_node;

/*
 * Directory entry returned by readdir.
 * name: filename (null-terminated)
 * inode: unique node identifier
 */
typedef struct {
    char     name[64];
    uint32_t inode;
    uint32_t flags;
} vfs_dirent_t;

/*
 * vfs_node_t – the universal "inode".
 *
 * Function pointers are NULL for unsupported operations.
 * impl_data is a private pointer used by each filesystem driver
 * (e.g., pointer to a file_t, or a device context).
 */
typedef struct vfs_node {
    char     name[64];           /* Node name (filename or device name) */
    uint32_t flags;              /* VFS_FLAG_* bitmask */
    uint32_t length;             /* File size in bytes (0 for devices) */
    uint32_t inode;              /* Unique identifier */
    void*    impl_data;          /* Private filesystem/device pointer */

    /* Operations – return bytes read/written, or 0/negative on error */
    int32_t  (*read) (struct vfs_node* node, uint32_t offset, uint32_t size, uint8_t* buf);
    int32_t  (*write)(struct vfs_node* node, uint32_t offset, uint32_t size, const uint8_t* buf);
    int32_t  (*open) (struct vfs_node* node, uint32_t flags);
    int32_t  (*close)(struct vfs_node* node);
    vfs_dirent_t* (*readdir)(struct vfs_node* node, uint32_t index);
    struct vfs_node* (*finddir)(struct vfs_node* node, const char* name);
} vfs_node_t;

/* ──────── Mount table entry ──────── */
#define VFS_MAX_MOUNTS 8

typedef struct {
    char        path[128];   /* Mount point path, e.g. "/" or "/dev" */
    vfs_node_t* root;        /* Root node of the mounted filesystem */
    int         in_use;
} vfs_mount_t;

/* ──────── Public API ──────── */

/* Initialize the VFS subsystem (must be called before any mount) */
void vfs_init(void);

/* Mount a filesystem root node at a given path */
int vfs_mount(const char* path, vfs_node_t* root);

/*
 * Open a file/device by absolute path.
 * Returns a vfs_node_t* that the caller can use for read/write/close,
 * or NULL on failure.
 */
vfs_node_t* vfs_open(const char* path, uint32_t flags);

/* Dispatch read through a node's function pointer */
int32_t vfs_read(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buf);

/* Dispatch write through a node's function pointer */
int32_t vfs_write(vfs_node_t* node, uint32_t offset, uint32_t size, const uint8_t* buf);

/* Dispatch close through a node's function pointer */
int32_t vfs_close(vfs_node_t* node);

/* Read directory entry at index from a directory node */
vfs_dirent_t* vfs_readdir(vfs_node_t* node, uint32_t index);

/* Lookup a child by name inside a directory node */
vfs_node_t* vfs_finddir(vfs_node_t* node, const char* name);

#endif /* VFS_H */
