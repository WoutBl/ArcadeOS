/*
 * ArcadeOS – Round-Robin Preemptive Scheduler
 *
 * Hooked into the PIT IRQ0 handler via clock.c.
 * Every SCHED_QUANTUM_MS milliseconds, schedule() picks the next
 * READY task and performs a context switch via switch_task().
 */

#include "scheduler.h"
#include "serial.h"
#include "vga.h"
#include "clock.h"
#include "syscall.h"

/* Current task tracking */
task_t*  current_task       = NULL;
int      current_task_index = 0;
int      scheduler_enabled  = 0;

/* Tick counter for quantum tracking */
static uint32_t sched_tick_count = 0;

/* ──────── Scheduler tick (called from PIT IRQ0 handler) ──────── */
void scheduler_tick(void) {
    if (!scheduler_enabled) return;
    if (num_tasks <= 1) return;     /* Only one task, nothing to switch */

    sched_tick_count++;
    if (sched_tick_count < SCHED_QUANTUM_MS) return;
    sched_tick_count = 0;

    schedule();
}

/* ──────── Pick next READY task and switch ──────── */
void schedule(void) {
    if (!scheduler_enabled) return;
    if (num_tasks <= 1) return;

    /* Find the next ready task (round-robin) */
    int old_index = current_task_index;
    int next_index = old_index;

    for (int i = 0; i < num_tasks; i++) {
        next_index = (old_index + 1 + i) % num_tasks;
        if (tasks[next_index].state == TASK_READY || tasks[next_index].state == TASK_RUNNING) {
            break;
        }
    }

    /* If we found the same task or no other ready task, don't switch */
    if (next_index == old_index) return;

    /* Mark old task as READY (if it was RUNNING) */
    task_t* old_task = &tasks[old_index];
    if (old_task->state == TASK_RUNNING) {
        old_task->state = TASK_READY;
    }

    /* Switch to new task */
    task_t* new_task = &tasks[next_index];


    /* Phase 3: Check pending signals before switching into this task */
    uint32_t active_signals = new_task->pending_signals & ~new_task->ignored_signals;
    if (active_signals & ((1 << SIGINT) | (1 << SIGKILL))) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("\n[SCHED] Terminated Task ");
        terminal_writedec(new_task->id);
        terminal_writestring(" by signal.\n");

        new_task->state = TASK_DEAD;
        new_task->pending_signals = 0;

        /* Wake up tasks waiting on this dead one */
        for (int w = 0; w < num_tasks; w++) {
            if (tasks[w].state == TASK_BLOCKED && tasks[w].wait_pid == new_task->id) {
                tasks[w].state = TASK_READY;
                tasks[w].wait_pid = 0;
            }
        }

        /* We killed the task we picked. Re-run schedule to pick another valid task. */
        schedule();
        return; 
    }

    new_task->state = TASK_RUNNING;

    current_task_index = next_index;
    current_task = new_task;

    /* Perform the actual context switch */
    uint64_t current_cr3;
    asm volatile("mov %%cr3, %0" : "=r"(current_cr3));
    
    /* Update TSS kernel stack for Ring 3 → 0 transitions */
    if (new_task->kernel_stack_top != 0) {
        tss_set_kernel_stack(new_task->kernel_stack_top);
    }

    /* Switch address space if necessary */
    if (new_task->cr3 != 0 && new_task->cr3 != current_cr3) {
        asm volatile("mov %0, %%cr3" :: "r"(new_task->cr3) : "memory");
    }

    switch_task(&old_task->esp, new_task->esp);
}

/* ──────── Initialize the scheduler ──────── */
void scheduler_init(void) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
    terminal_writestring("[SCHED] Initializing scheduler...\n");

    /* Initialize the task subsystem */
    task_init();

    /*
     * Task 0: the current execution context (kernel_main → shell_run).
     * We don't need to allocate a stack for it – it's already running
     * on the boot stack. Its ESP will be saved when the first context
     * switch happens.
     */
    task_t* main_task = &tasks[0];
    main_task->id    = 0;
    main_task->state = TASK_RUNNING;
    main_task->entry = NULL;   /* Already running */
    main_task->stack_base = NULL;  /* Boot stack, not from kmalloc */
    main_task->stack_size = 0;
    extern uint64_t stack_top;
    main_task->kernel_stack_top = (uint64_t)&stack_top;

    /* Read current CR3 */
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    main_task->cr3 = cr3;

    strcpy(main_task->name, "kernel/shell");

    num_tasks = 1;
    current_task_index = 0;
    current_task = main_task;

    /* Enable the scheduler */
    scheduler_enabled = 1;
    sched_tick_count = 0;

    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("[SCHED] Ready (");
    terminal_writedec(SCHED_QUANTUM_MS);
    terminal_writestring("ms quantum)\n");
}

/* ──────── Dump scheduler info ──────── */
void scheduler_dump_info(void) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("Scheduler Information:\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));

    terminal_writestring("  Quantum:     ");
    terminal_writedec(SCHED_QUANTUM_MS);
    terminal_writestring(" ms\n");

    terminal_writestring("  Total tasks: ");
    terminal_writedec((uint32_t)num_tasks);
    terminal_writestring("\n\n");

    const char* state_names[] = { "RUNNING", "READY", "BLOCKED", "DEAD" };

    for (int i = 0; i < num_tasks; i++) {
        task_t* t = &tasks[i];

        if (i == current_task_index) {
            terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
            terminal_writestring("  > ");
        } else {
            terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
            terminal_writestring("    ");
        }

        terminal_writestring("Task ");
        terminal_writedec(t->id);
        terminal_writestring(": ");
        terminal_writestring(t->name);
        terminal_writestring(" [");
        terminal_writestring(state_names[t->state]);
        terminal_writestring("] ESP=0x");
        terminal_writehex(t->esp);
        terminal_writestring("\n");
    }
}
