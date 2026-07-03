/*
 * ArcadeOS – VFS Core
 *
 * Mount table + path resolution + dispatch wrappers.
 */

#include "vfs.h"
#include "vga.h"

/* ──────── Mount table ──────── */
static vfs_mount_t mount_table[VFS_MAX_MOUNTS];
static int         num_mounts = 0;

/* ──────── Init ──────── */
void vfs_init(void) {
    memset(mount_table, 0, sizeof(mount_table));
    num_mounts = 0;
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("[VFS] Initialized\n");
}

/* ──────── Mount ──────── */
int vfs_mount(const char* path, vfs_node_t* root) {
    if (!path || !root || num_mounts >= VFS_MAX_MOUNTS) return -1;

    vfs_mount_t* m = &mount_table[num_mounts++];
    strncpy(m->path, path, sizeof(m->path) - 1);
    m->path[sizeof(m->path) - 1] = '\0';
    m->root   = root;
    m->in_use = 1;

    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("[VFS] Mounted '");
    terminal_writestring(root->name);
    terminal_writestring("' at ");
    terminal_writestring(path);
    terminal_writestring("\n");
    return 0;
}

/*
 * ──────── Path Resolution ────────
 *
 * Given an absolute path like "/dev/tty":
 *   1. Find the longest-matching mount point (e.g. "/dev")
 *   2. Get the root node of that mount
 *   3. Walk the remaining path components through finddir()
 */
vfs_node_t* vfs_open(const char* path, uint32_t flags) {
    if (!path || path[0] != '/') return (vfs_node_t*)0;

    /* 1. Find best (longest) matching mount point */
    int   best_idx    = -1;
    int   best_len    = -1;
    for (int i = 0; i < num_mounts; i++) {
        if (!mount_table[i].in_use) continue;
        int mlen = (int)strlen(mount_table[i].path);
        if (strncmp(path, mount_table[i].path, (uint32_t)mlen) == 0) {
            /* The path must end here OR have a '/' separator next */
            char next = path[mlen];
            if (next == '\0' || next == '/') {
                if (mlen > best_len) {
                    best_len = mlen;
                    best_idx = i;
                }
            }
        }
    }
    if (best_idx < 0) return (vfs_node_t*)0;

    vfs_node_t* node = mount_table[best_idx].root;

    /* 2. Walk remaining path components */
    const char* remainder = path + best_len;
    while (*remainder == '/') remainder++;   /* skip leading slash */

    while (*remainder != '\0' && node != (vfs_node_t*)0) {
        /* Extract one component */
        char component[64];
        int  clen = 0;
        while (remainder[clen] != '\0' && remainder[clen] != '/' && clen < 63) {
            component[clen] = remainder[clen];
            clen++;
        }
        component[clen] = '\0';
        remainder += clen;
        while (*remainder == '/') remainder++;

        /* Descend into the directory */
        if (node->finddir) {
            node = node->finddir(node, component);
        } else {
            node = (vfs_node_t*)0;
        }
    }

    /* 3. Call open() on the resolved node */
    if (node && node->open) {
        node->open(node, flags);
    }

    return node;
}

/* ──────── Dispatch wrappers ──────── */

int32_t vfs_read(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buf) {
    if (!node || !node->read) return -1;
    return node->read(node, offset, size, buf);
}

int32_t vfs_write(vfs_node_t* node, uint32_t offset, uint32_t size, const uint8_t* buf) {
    if (!node || !node->write) return -1;
    return node->write(node, offset, size, buf);
}

int32_t vfs_close(vfs_node_t* node) {
    if (!node || !node->close) return 0;  /* close is optional */
    return node->close(node);
}

vfs_dirent_t* vfs_readdir(vfs_node_t* node, uint32_t index) {
    if (!node || !node->readdir) return (vfs_dirent_t*)0;
    return node->readdir(node, index);
}

vfs_node_t* vfs_finddir(vfs_node_t* node, const char* name) {
    if (!node || !node->finddir) return (vfs_node_t*)0;
    return node->finddir(node, name);
}
