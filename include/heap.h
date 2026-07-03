#ifndef HEAP_H
#define HEAP_H

#include "types.h"

#define HEAP_INITIAL_PAGES 256   /* 256 pages × 4 KiB = 1 MiB initial heap */

/* Public API */
void  heap_init(void);
void* kmalloc(size_t size);
void  kfree(void* ptr);
void  heap_dump_info(void);   /* Print stats to terminal */

#endif /* HEAP_H */
