/*
 * ArcadeOS – Pipe VFS Node Implementation
 *
 * Ring-buffer pipe with cooperative blocking:
 * when empty, readers yield; when full, writers yield.
 */

#include "pipe.h"
#include "heap.h"
#include "scheduler.h"
#include "task.h"
#include "vga.h"

/* ──────── Helpers ──────── */

/* Round up: number of bytes readable without wrap */
static inline uint32_t pipe_readable(const pipe_buf_t* pb) {
    return pb->count;
}

static inline uint32_t pipe_writable(const pipe_buf_t* pb) {
    return PIPE_BUF_SIZE - pb->count;
}

/* ──────── Dynamic FD Scanning ──────── */
/* 
 * Since our OS doesn't increment FD refcounts on spawn() or decrement on exit(),
 * we scan all active tasks to see if any holds a reference to this pipe end.
 */
static int pipe_has_writers(pipe_buf_t* pb) {
    for (int i = 0; i < num_tasks; i++) {
        if (tasks[i].state != TASK_DEAD) {
            for (int fd = 0; fd < MAX_FD; fd++) {
                vfs_node_t* n = tasks[i].fds[fd];
                if (n && (n->flags & VFS_FLAG_PIPE_W) && n->impl_data == (void*)pb) {
                    return 1;
                }
            }
        }
    }
    return 0;
}

static int pipe_has_readers(pipe_buf_t* pb) {
    for (int i = 0; i < num_tasks; i++) {
        if (tasks[i].state != TASK_DEAD) {
            for (int fd = 0; fd < MAX_FD; fd++) {
                vfs_node_t* n = tasks[i].fds[fd];
                if (n && (n->flags & VFS_FLAG_PIPE_R) && n->impl_data == (void*)pb) {
                    return 1;
                }
            }
        }
    }
    return 0;
}

/* ──────── Read operation ──────── */
/*
 * Block until at least 1 byte is available or the write-end is closed.
 * Then copy up to 'size' bytes into buf.
 */
static int32_t pipe_read(vfs_node_t* node, uint32_t offset __attribute__((unused)),
                          uint32_t size, uint8_t* buf) {
    pipe_buf_t* pb = (pipe_buf_t*)node->impl_data;
    if (!pb || !buf || size == 0) return 0;

    /* Block while buffer is empty *and* there are still writers */
    while (pipe_readable(pb) == 0) {
        if (!pipe_has_writers(pb)) {
            /* Write-end closed by all tasks and buffer drained → EOF */
            return 0;
        }
        /* Yield cooperatively; the writer will fill the buffer */
        if (current_task) {
            schedule();
        }
    }

    /* Copy as many bytes as available (up to 'size') */
    uint32_t bytes_read = 0;
    while (bytes_read < size && pb->count > 0) {
        buf[bytes_read++] = pb->buf[pb->read_pos];
        pb->read_pos = (pb->read_pos + 1) % PIPE_BUF_SIZE;
        pb->count--;
    }
    return (int32_t)bytes_read;
}

/* ──────── Write operation ──────── */
/*
 * Block until there is space in the buffer or all readers closed.
 * Then copy as many bytes as fit (up to 'size') into the ring.
 */
static int32_t pipe_write(vfs_node_t* node, uint32_t offset __attribute__((unused)),
                           uint32_t size, const uint8_t* buf) {
    pipe_buf_t* pb = (pipe_buf_t*)node->impl_data;
    if (!pb || !buf) return -1;
    if (size == 0) return 0;

    /* EPIPE: no readers left */
    if (!pipe_has_readers(pb)) return -1;

    uint32_t bytes_written = 0;
    while (bytes_written < size) {
        /* Block while buffer is full */
        while (pipe_writable(pb) == 0) {
            if (!pipe_has_readers(pb)) return (int32_t)bytes_written;  /* EPIPE */
            if (current_task) {
                schedule();
            }
        }

        /* Write one byte at a time */
        pb->buf[pb->write_pos] = buf[bytes_written++];
        pb->write_pos = (pb->write_pos + 1) % PIPE_BUF_SIZE;
        pb->count++;
    }
    return (int32_t)bytes_written;
}

/* ──────── Close operations ──────── */

static int32_t pipe_close_read(vfs_node_t* node) {
    pipe_buf_t* pb = (pipe_buf_t*)node->impl_data;
    if (!pb) return 0;
    if (pb->open_readers > 0) pb->open_readers--;
    /* Don't free pb here – write-end may still be alive */
    return 0;
}

static int32_t pipe_close_write(vfs_node_t* node) {
    pipe_buf_t* pb = (pipe_buf_t*)node->impl_data;
    if (!pb) return 0;
    if (pb->open_writers > 0) pb->open_writers--;
    /* Unblock any reader polling on empty buffer with no more data */
    return 0;
}

/* ──────── Public factory functions ──────── */

pipe_buf_t* pipe_create(void) {
    pipe_buf_t* pb = (pipe_buf_t*)kmalloc(sizeof(pipe_buf_t));
    if (!pb) return (pipe_buf_t*)0;
    memset(pb, 0, sizeof(pipe_buf_t));
    pb->open_readers = 1;
    pb->open_writers = 1;
    return pb;
}

vfs_node_t* pipe_make_read_node(pipe_buf_t* pb) {
    if (!pb) return (vfs_node_t*)0;
    vfs_node_t* n = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    if (!n) return (vfs_node_t*)0;
    memset(n, 0, sizeof(vfs_node_t));
    strncpy(n->name, "pipe:r", sizeof(n->name) - 1);
    n->flags     = VFS_FLAG_DEVICE | VFS_FLAG_PIPE_R;
    n->impl_data = (void*)pb;
    n->read      = pipe_read;
    n->write     = (int32_t (*)(struct vfs_node*, uint32_t, uint32_t, const uint8_t*))0;
    n->close     = pipe_close_read;
    return n;
}

vfs_node_t* pipe_make_write_node(pipe_buf_t* pb) {
    if (!pb) return (vfs_node_t*)0;
    vfs_node_t* n = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    if (!n) return (vfs_node_t*)0;
    memset(n, 0, sizeof(vfs_node_t));
    strncpy(n->name, "pipe:w", sizeof(n->name) - 1);
    n->flags     = VFS_FLAG_DEVICE | VFS_FLAG_PIPE_W;
    n->impl_data = (void*)pb;
    n->read      = (int32_t (*)(struct vfs_node*, uint32_t, uint32_t, uint8_t*))0;
    n->write     = pipe_write;
    n->close     = pipe_close_write;
    return n;
}
