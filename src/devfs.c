/*
 * ArcadeOS – Device Filesystem (devfs)
 *
 * Mounts special device nodes at /dev.
 *
 *   /dev/tty   – the system terminal (keyboard in, VGA out)
 *   /dev/null  – the null device (writes silently discarded, reads return EOF)
 */

#include "devfs.h"
#include "vfs.h"
#include "keyboard.h"
#include "vga.h"

/* ──────── Static node storage ──────── */

/* We need nodes for: /dev root dir, /dev/tty, /dev/null */
static vfs_node_t devfs_root_node;
static vfs_node_t tty_node;
static vfs_node_t null_node;

/* Directory entry cache for readdir */
static vfs_dirent_t devfs_dirents[2];

/* ──────── /dev/tty operations ──────── */

static int32_t tty_read(vfs_node_t* node __attribute__((unused)),
                         uint32_t offset __attribute__((unused)),
                         uint32_t size, uint8_t* buf) {
    if (!buf || size == 0) return 0;

    uint32_t bytes_read = 0;
    char* cbuf = (char*)buf;

    while (bytes_read < size) {
        char c = keyboard_read_blocking();

        /* Handle backspace in the kernel buffer echo */
        if (c == '\b') {
            if (bytes_read > 0) {
                bytes_read--;
                terminal_putchar('\b');
                terminal_putchar(' ');
                terminal_putchar('\b');
            }
            continue;
        }

        /* Echo the character */
        terminal_putchar(c);

        cbuf[bytes_read++] = c;
        if (c == '\n') break;   /* Line-buffered input */
    }
    return (int32_t)bytes_read;
}

static int32_t tty_write(vfs_node_t* node __attribute__((unused)),
                          uint32_t offset __attribute__((unused)),
                          uint32_t size, const uint8_t* buf) {
    if (!buf) return 0;
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    /* Write exactly 'size' bytes; treat them as a null-terminated string */
    if (size == 0) {
        terminal_writestring((const char*)buf);
    } else {
        /* Write char-by-char to respect 'size' if no null terminator */
        for (uint32_t i = 0; i < size; i++) {
            char ch[2] = { (char)buf[i], '\0' };
            terminal_writestring(ch);
        }
    }
    return (int32_t)size;
}

/* ──────── /dev/null operations ──────── */

static int32_t null_read(vfs_node_t* node __attribute__((unused)),
                          uint32_t offset __attribute__((unused)),
                          uint32_t size __attribute__((unused)),
                          uint8_t* buf __attribute__((unused))) {
    return 0;   /* EOF immediately */
}

static int32_t null_write(vfs_node_t* node __attribute__((unused)),
                           uint32_t offset __attribute__((unused)),
                           uint32_t size, const uint8_t* buf __attribute__((unused))) {
    return (int32_t)size;  /* Pretend every byte was written */
}

/* ──────── /dev directory operations ──────── */

static vfs_dirent_t* devfs_readdir(vfs_node_t* node __attribute__((unused)),
                                    uint32_t index) {
    if (index == 0) {
        strncpy(devfs_dirents[0].name, "tty",  sizeof(devfs_dirents[0].name) - 1);
        devfs_dirents[0].inode = tty_node.inode;
        devfs_dirents[0].flags = VFS_FLAG_DEVICE;
        return &devfs_dirents[0];
    } else if (index == 1) {
        strncpy(devfs_dirents[1].name, "null", sizeof(devfs_dirents[1].name) - 1);
        devfs_dirents[1].inode = null_node.inode;
        devfs_dirents[1].flags = VFS_FLAG_DEVICE;
        return &devfs_dirents[1];
    }
    return (vfs_dirent_t*)0;
}

static vfs_node_t* devfs_finddir(vfs_node_t* node __attribute__((unused)),
                                  const char* name) {
    if (strcmp(name, "tty")  == 0) return &tty_node;
    if (strcmp(name, "null") == 0) return &null_node;
    return (vfs_node_t*)0;
}

/* ──────── Public init ──────── */

void devfs_init(void) {
    /* /dev/tty */
    memset(&tty_node, 0, sizeof(vfs_node_t));
    strncpy(tty_node.name, "tty",  sizeof(tty_node.name) - 1);
    tty_node.flags    = VFS_FLAG_DEVICE;
    tty_node.inode    = 1;
    tty_node.read     = tty_read;
    tty_node.write    = tty_write;

    /* /dev/null */
    memset(&null_node, 0, sizeof(vfs_node_t));
    strncpy(null_node.name, "null", sizeof(null_node.name) - 1);
    null_node.flags   = VFS_FLAG_DEVICE;
    null_node.inode   = 2;
    null_node.read    = null_read;
    null_node.write   = null_write;

    /* /dev root directory */
    memset(&devfs_root_node, 0, sizeof(vfs_node_t));
    strncpy(devfs_root_node.name, "dev",  sizeof(devfs_root_node.name) - 1);
    devfs_root_node.flags   = VFS_FLAG_DIR;
    devfs_root_node.inode   = 0;
    devfs_root_node.readdir = devfs_readdir;
    devfs_root_node.finddir = devfs_finddir;

    /* Mount /dev */
    vfs_mount("/dev", &devfs_root_node);

    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("[DEVFS] /dev/tty and /dev/null ready\n");
}
