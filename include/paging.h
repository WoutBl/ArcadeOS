#ifndef PAGING_H
#define PAGING_H

#include "types.h"

/* ──────── Page sizes ──────── */
#define PAGE_SIZE_4K     4096
#define PAGES_PER_TABLE  1024
#define PAGES_PER_DIR    1024

/* Each Page Directory Entry covers 4 MiB (1024 pages × 4 KiB) */
#define PDE_COVER        (PAGES_PER_TABLE * PAGE_SIZE_4K)

/* ──────── Page Table Entry / Page Directory Entry flags ──────── */
#define PTE_PRESENT      0x001   /* Page is present in physical memory */
#define PTE_READ_WRITE   0x002   /* Page is writable (0 = read-only) */
#define PTE_USER         0x004   /* Page is accessible from ring 3 */
#define PTE_WRITE_THRU   0x008   /* Write-through caching */
#define PTE_CACHE_DIS    0x010   /* Disable caching */
#define PTE_ACCESSED     0x020   /* Set by CPU when page is accessed */
#define PTE_DIRTY        0x040   /* Set by CPU when page is written to */
#define PTE_4MB          0x080   /* 4 MiB page (PDE only) */
#define PTE_GLOBAL       0x100   /* Global page (not flushed on CR3 reload) */

/* Extract physical address from a PDE/PTE (mask off lower 12 flag bits) */
#define PTE_FRAME_MASK   0xFFFFF000

/* ──────── Public API ──────── */
void paging_init(void);
void paging_dump_info(void);

/* Accessor for the master kernel directory */
uint32_t* paging_get_kernel_pd(void);

/* Map a single virtual page to a physical frame.
 * flags = PTE_PRESENT | PTE_READ_WRITE | etc. */
void paging_map_page(uint32_t virtual_addr, uint32_t physical_addr, uint32_t flags);

/* Map a page into a specific page directory (used by loader) */
void paging_map_page_to(uint32_t* pd, uint32_t vaddr, uint32_t paddr, uint32_t flags);

/*
 * Clone the kernel's page directory into a fresh one suitable for a
 * new user process.  All kernel PDEs are copied (supervisor-only, shared
 * physical page tables) so interrupts / syscalls always see the kernel.
 * Returns the *physical* address of the new PD (ready to load into CR3).
 */
uint32_t paging_clone_kernel_pd(void);

/* Unmap a single virtual page */
void paging_unmap_page(uint32_t virtual_addr);

#endif /* PAGING_H */
