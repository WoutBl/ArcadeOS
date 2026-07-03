/*
 * ArcadeOS – Task Management (kernel threads)
 */

#include "task.h"
#include "scheduler.h"
#include "heap.h"
#include "vga.h"

/* Task table */
task_t tasks[MAX_TASKS];
int    num_tasks = 0;

static uint32_t next_task_id = 0;

/*
 * Bootstrap wrapper for new tasks.
 * When switch_task "returns" into a new task for the first time,
 * it jumps here. We enable interrupts (since we came from an IRQ
 * context where IF was cleared) and call the task's entry point.
 */
static void task_bootstrap(void) {
    sti();  /* Re-enable interrupts */
    current_task->entry();

    /* If the entry function returns, mark the task as dead */
    current_task->state = TASK_DEAD;

    /* Yield to the scheduler – this task will never be picked again */
    for (;;) {
        schedule();
        hlt();
    }
}

/* ──────── Public API ──────── */

void task_init(void) {
    memset(tasks, 0, sizeof(tasks));
    num_tasks = 0;
    next_task_id = 0;
}

int create_kernel_thread(void (*entry)(void), const char* name) {
    /*
     * Recycle a dead slot first – a console launches and quits games
     * indefinitely, so the table must not fill up with corpses.
     * (Slot 0 is never recycled; it is the kernel bootstrap context.)
     */
    int idx = -1;
    for (int i = 1; i < num_tasks; i++) {
        if (tasks[i].state == TASK_DEAD) {
            idx = i;
            break;
        }
    }

    if (idx < 0) {
        if (num_tasks >= MAX_TASKS) {
            terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
            terminal_writestring("[TASK] Error: Max tasks reached\n");
            return -1;
        }
        idx = num_tasks++;
    } else {
        /* Release the dead task's kernel stack before reusing the slot */
        if (tasks[idx].stack_base)
            kfree(tasks[idx].stack_base);
        memset(&tasks[idx], 0, sizeof(task_t));
    }

    task_t* task = &tasks[idx];

    task->id    = next_task_id++;
    task->state = TASK_READY;
    task->entry = entry;

    /* Copy the name */
    size_t i = 0;
    while (name[i] && i < 31) { task->name[i] = name[i]; i++; }
    task->name[i] = '\0';

    task->pending_signals = 0;
    task->ignored_signals = 0;

    /* Read the current CR3 (all kernel tasks share the same page directory) */
    uint32_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    task->cr3 = cr3;

    /* Allocate a stack */
    task->stack_size = TASK_STACK_SIZE;
    task->stack_base = (uint8_t*)kmalloc(TASK_STACK_SIZE);
    if (task->stack_base == NULL) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("[TASK] Error: Could not allocate stack\n");
        return -1;
    }

    /*
     * Set up the initial stack so it looks like switch_task was called:
     *
     * Top of stack (high addresses):
     *   [task_bootstrap]  ← return address for switch_task's 'ret'
     *   [0]               ← fake EBP
     *   [0]               ← fake EBX
     *   [0]               ← fake ESI
     *   [0]               ← fake EDI   ← task->esp points HERE
     */
    uint32_t* stack_top = (uint32_t*)(task->stack_base + TASK_STACK_SIZE);

    *(--stack_top) = (uint32_t)task_bootstrap; /* Return address */
    *(--stack_top) = 0;                        /* EBP */
    *(--stack_top) = 0;                        /* EBX */
    *(--stack_top) = 0;                        /* ESI */
    *(--stack_top) = 0;                        /* EDI */

    task->esp = (uint32_t)stack_top;
    /*
     * kernel_stack_top must be the HIGHEST address of this task's kernel stack.
     * The TSS.esp0 field is loaded with this value on every context switch so
     * that the CPU knows where to put the kernel stack frame when a Ring 3 → 0
     * transition happens (interrupt / syscall).  It must NOT be the saved ESP
     * (which decreases as the stack grows), but the fixed top-of-stack address.
     */
    task->kernel_stack_top = (uint32_t)(task->stack_base + TASK_STACK_SIZE);

    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("[TASK] Created: ");
    terminal_writestring(task->name);
    terminal_writestring(" (ID ");
    terminal_writedec(task->id);
    terminal_writestring(")\n");

    return idx;
}

/*
 * task_open_std_fds – wire FD 0/1/2 to /dev/tty for a user process.
 *
 * Called by loader.c immediately after an ELF is exec'd so that
 * any write(1, ...) or read(0, ...) goes through the VFS devfs driver.
 * All three FDs point to the same /dev/tty node (it is stateless).
 */
void task_open_std_fds(task_t* task) {
    if (!task) return;

    vfs_node_t* tty = vfs_open("/dev/tty", 0);
    if (!tty) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("[TASK] Warning: /dev/tty not found – FDs not wired\n");
        return;
    }

    /* Only wire if they haven't been forcefully inherited (e.g. pipes on spawn) */
    if (!task->fds[STDIN_FD])  task->fds[STDIN_FD]  = tty;
    if (!task->fds[STDOUT_FD]) task->fds[STDOUT_FD] = tty;
    if (!task->fds[STDERR_FD]) task->fds[STDERR_FD] = tty;
}

/*
 * Deliver a signal to the foreground process.
 * We define the foreground process as the one the init shell (tasks[0]) is currently waiting for.
 */
void task_kill_foreground(int sig) {
    /* Safety check */
    if (num_tasks <= 1) return;
    
    /* Assuming tasks[0] is the shell... */
    task_t* shell = &tasks[0];
    
    if (shell->state == TASK_BLOCKED && shell->wait_pid > 0) {
        /* Shell is blocked waiting for a child process. Send signal to that child. */
        uint32_t fg_pid = shell->wait_pid;
        for (int i = 0; i < num_tasks; i++) {
            if (tasks[i].id == fg_pid && tasks[i].state != TASK_DEAD) {
                tasks[i].pending_signals |= (1 << sig);
                
                terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
                terminal_writestring("\n[SIGNAL] Delivered SIG ");
                terminal_writedec(sig);
                terminal_writestring(" to PID ");
                terminal_writedec(fg_pid);
                terminal_writestring("\n");
                break;
            }
        }
    }
}
