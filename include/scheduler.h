#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "types.h"
#include "task.h"

/* Time quantum: switch tasks every SCHED_QUANTUM_MS milliseconds */
#define SCHED_QUANTUM_MS 20

/* Public API */
void scheduler_init(void);
void scheduler_tick(void);              /* Called from PIT IRQ0 handler */
void schedule(void);                    /* Pick next task and switch */
void scheduler_dump_info(void);         /* Print scheduler stats */

/* Current task pointer */
extern task_t*  current_task;
extern int      current_task_index;
extern int      scheduler_enabled;

/* Assembly context switch (defined in switch.asm) */
extern void switch_task(uint32_t* old_esp_ptr, uint32_t new_esp);

#endif /* SCHEDULER_H */
