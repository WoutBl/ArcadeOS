#ifndef PAGING_H
#define PAGING_H

#include "types.h"

/* ──────── Page sizes ──────── */
#define PAGE_SIZE_4K     4096
#define PAGE_SIZE_2M     0x200000
#define ENTRIES_PER_TABLE 512

/* ──────── Page table entry flags (all levels) ──────── */
#define PTE_PRESENT      0x001   /* Page is present in physical memory */
#define PTE_READ_WRITE   0x002   /* Page is writable (0 = read-only) */
#define PTE_USER         0x004   /* Page is accessible from ring 3 */
#define PTE_WRITE_THRU   0x008   /* Write-through caching */
#define PTE_CACHE_DIS    0x010   /* Disable caching */
#define PTE_ACCESSED     0x020   /* Set by CPU when page is accessed */
#define PTE_DIRTY        0x040   /* Set by CPU when page is written to */
#define PTE_PS           0x080   /* Large page (2 MiB in a PD entry) */
#define PTE_GLOBAL       0x100   /* Global page (not flushed on CR3 reload) */
#define PTE_NX           (1ULL << 63) /* No-execute (requires EFER.NXE) */

/* Extract physical address from an entry (bits 12-51) */
#define PTE_FRAME_MASK   0x000FFFFFFFFFF000ULL

/* ──────── Public API ──────── */
void paging_init(void);
void paging_dump_info(void);

/* Accessor for the master kernel PML4 */
uint64_t* paging_get_kernel_pd(void);

/* PTE_NX if the CPU supports no-execute and EFER.NXE is enabled, else 0.
 * OR this into user PTE flags — never use PTE_NX directly, because on a
 * CPU without NX bit 63 is reserved and faults. */
uint64_t paging_nx_flag(void);

/*
 * Syscall pointer validation (the copy_from_user substitute).
 *
 * Walks the CURRENT address space and verifies every page of
 * [vaddr, vaddr+len) is mapped PRESENT|USER at every level (plus
 * writable at the leaf when write != 0). A bare range check is NOT
 * enough: user PML4s share the kernel identity map, so a game could
 * otherwise hand the kernel a pointer at kernel heap/page tables and
 * have a syscall read or write it with supervisor rights.
 */
int paging_user_access_ok(uint64_t vaddr, uint64_t len, int write);

/* Same, for a NUL-terminated string: verifies page-by-page and scans
 * for the terminator. Returns the string length, or -1 if any byte up
 * to (and including) the NUL is not user-accessible or no NUL is found
 * within maxlen bytes. */
long paging_user_str_ok(const char* s, uint64_t maxlen);

/* Identity-map a 2 MiB-aligned MMIO region into the kernel hierarchy
 * (cache-disabled). Used for device register windows discovered after
 * paging_init(), e.g. the AHCI ABAR. */
void paging_kernel_map_mmio(uint64_t phys, uint64_t size);

/* Map a 4 KiB page into a specific process PML4 (used by the loader).
 * Descends the 4-level hierarchy, privately copying any table still
 * shared with the kernel, and replaces 2 MiB kernel identity mappings
 * with process-private 4 KiB page tables (overlay semantics). */
void paging_map_page_to(uint64_t* pml4, uint64_t vaddr, uint64_t paddr, uint64_t flags);

/*
 * Clone the kernel's PML4 into a fresh one suitable for a new user
 * process.  All entries are copied verbatim (shared kernel tables);
 * user overlays are split off lazily by paging_map_page_to().
 * Returns the *physical* address of the new PML4 (ready for CR3).
 */
uint64_t paging_clone_kernel_pd(void);

#endif /* PAGING_H */
