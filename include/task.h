#ifndef TASK_H
#define TASK_H

#include "types.h"
#include "vfs.h"

/* POSIX Signals */
#define SIGINT   2
#define SIGKILL  9

#define SIG_DFL  0
#define SIG_IGN  1

/* Task states */
typedef enum {
    TASK_RUNNING,
    TASK_READY,
    TASK_BLOCKED,
    TASK_DEAD
} task_state_t;

/* Task Control Block */
typedef struct {
    uint32_t      id;
    task_state_t  state;
    uint32_t      wait_pid;      /* PID to wait for if blocked */
    uint64_t      esp;           /* Saved stack pointer (RSP) */
    uint64_t      cr3;           /* PML4 (same for all kernel tasks) */
    void          (*entry)(void);/* Entry point function */
    uint8_t*      stack_base;    /* Base of allocated stack (for kfree) */
    uint32_t      stack_size;    /* Size of the stack */
    uint64_t      kernel_stack_top; /* RSP0 for TSS */
    char          name[32];      /* Human-readable task name */
    char**        cmdline_args;  /* Deep copy of argv for exec() to push to user stack */
    
    uint32_t      pending_signals;
    uint32_t      ignored_signals;

    /*
     * File Descriptor Table.
     * fds[0] = stdin, fds[1] = stdout, fds[2] = stderr.
     * NULL slots are free.
     */
    vfs_node_t*   fds[MAX_FD];
} task_t;

#define MAX_TASKS       16
#define TASK_STACK_SIZE  8192    /* 8 KiB per task stack */

/* Public API */
void task_init(void);
int  create_kernel_thread(void (*entry)(void), const char* name);

/*
 * Wire FD 0 (stdin), 1 (stdout), 2 (stderr) to /dev/tty for a user task.
 * Called by loader.c after every exec().
 */
void task_open_std_fds(task_t* task);

/* Signal Handling */
void task_kill_foreground(int sig);

/* Access to the task table */
extern task_t  tasks[MAX_TASKS];
extern int     num_tasks;

#endif /* TASK_H */
