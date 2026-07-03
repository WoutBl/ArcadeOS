/*
 * ArcadeOS – Pipe VFS Node
 *
 * A pipe is a uni-directional, in-kernel FIFO implemented as a 4 KiB ring
 * buffer.  Two VFS nodes share the same pipe_buf_t:
 *
 *   read-node  (flags = VFS_FLAG_PIPE_R)  –  only pipe_read() works
 *   write-node (flags = VFS_FLAG_PIPE_W)  –  only pipe_write() works
 *
 * Blocking semantics (cooperative multitasking):
 *   read  on empty buffer → yield until writer writes or write-end closes
 *   write on full  buffer → yield until reader drains or read-end closes
 */

#ifndef PIPE_H
#define PIPE_H

#include "types.h"
#include "vfs.h"

/* ──────── Constants ──────── */
#define PIPE_BUF_SIZE  4096   /* Ring buffer capacity (bytes) */

/* Extra VFS flags for pipe nodes */
#define VFS_FLAG_PIPE_R  0x10   /* Read-only pipe end */
#define VFS_FLAG_PIPE_W  0x20   /* Write-only pipe end */

/* ──────── Pipe buffer (shared between both ends) ──────── */
typedef struct {
    uint8_t  buf[PIPE_BUF_SIZE]; /* The ring buffer itself          */
    uint32_t read_pos;           /* Next byte to consume            */
    uint32_t write_pos;          /* Next free slot to write into    */
    uint32_t count;              /* Bytes currently in the buffer   */
    int      open_readers;       /* Number of open read-end FDs     */
    int      open_writers;       /* Number of open write-end FDs    */
} pipe_buf_t;

/* ──────── Public API ──────── */

/*
 * Allocate and initialise a new pipe buffer.
 * Returns NULL on kmalloc failure.
 */
pipe_buf_t* pipe_create(void);

/*
 * Build a read-end VFS node backed by the given pipe buffer.
 * Returns NULL on kmalloc failure.
 */
vfs_node_t* pipe_make_read_node(pipe_buf_t* pb);

/*
 * Build a write-end VFS node backed by the given pipe buffer.
 * Returns NULL on kmalloc failure.
 */
vfs_node_t* pipe_make_write_node(pipe_buf_t* pb);

#endif /* PIPE_H */
