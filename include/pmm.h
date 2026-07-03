#ifndef PMM_H
#define PMM_H

#include "types.h"
#include "multiboot.h"

/* Page size: 4 KiB */
#define PAGE_SIZE 4096

/* Public API */
void      pmm_init(multiboot_info_t* mboot_info);
uint32_t  pmm_alloc_page(void);    /* Returns physical address of a free 4K page, or 0 on failure */
uint32_t  pmm_alloc_pages(uint32_t count);  /* Contiguous run of pages (for DMA/back buffers) */
void      pmm_free_page(uint32_t addr);
void      pmm_free_pages(uint32_t addr, uint32_t count);
void      pmm_dump_info(void);     /* Print stats to terminal */

/* Statistics */
uint32_t  pmm_get_total_pages(void);
uint32_t  pmm_get_used_pages(void);
uint32_t  pmm_get_free_pages(void);

#endif /* PMM_H */
