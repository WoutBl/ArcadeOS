/*
 * ArcadeOS – x86-64 4-level Paging (Virtual Memory)
 *
 * Identity-maps all physical RAM (and the framebuffer MMIO region) with
 * 2 MiB pages so virtual == physical, replacing the boot page tables the
 * bootloader set up. User processes get a cloned PML4; the loader overlays
 * 4 KiB user pages on top via paging_map_page_to(), which privately copies
 * any table still shared with the kernel before modifying it.
 * ISR 14 is hooked as a Page Fault handler.
 */

#include "paging.h"
#include "pmm.h"
#include "idt.h"
#include "vga.h"
#include "fb.h"
#include "task.h"
#include "scheduler.h"
#include "klog.h"

/* The kernel PML4 (512 entries × 8 bytes = 4 KiB) */
static uint64_t* kernel_pml4 = NULL;

/* Stats for dump_info */
static uint64_t mapped_2m_pages = 0;

/* PTE_NX once EFER.NXE is on, 0 on CPUs without NX (bit 63 reserved) */
static uint64_t pte_nx = 0;

uint64_t paging_nx_flag(void) {
    return pte_nx;
}

/* Enable no-execute support: CPUID leaf 0x80000001 EDX bit 20, then
 * EFER.NXE (MSR 0xC0000080 bit 11). Must happen before the first PTE
 * with bit 63 is installed, or the CPU faults on a reserved bit. */
static void nx_enable(void) {
    uint32_t a, b, c, d;
    asm volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                         : "a"(0x80000000u));
    if (a < 0x80000001u) return;
    asm volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                         : "a"(0x80000001u));
    if (!(d & (1u << 20))) return;

    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080u));
    lo |= (1u << 11);                       /* EFER.NXE */
    asm volatile("wrmsr" :: "a"(lo), "d"(hi), "c"(0xC0000080u));
    pte_nx = PTE_NX;
}

uint64_t* paging_get_kernel_pd(void) {
    return kernel_pml4;
}

/* ──────── CR register helpers ──────── */

static inline uint64_t read_cr2(void) {
    uint64_t val;
    asm volatile("mov %%cr2, %0" : "=r"(val));
    return val;
}

static inline void write_cr3(uint64_t pml4_phys) {
    asm volatile("mov %0, %%cr3" :: "r"(pml4_phys) : "memory");
}

/* ──────── Table walk helpers ──────── */

static inline int idx_pml4(uint64_t v) { return (int)((v >> 39) & 0x1FF); }
static inline int idx_pdpt(uint64_t v) { return (int)((v >> 30) & 0x1FF); }
static inline int idx_pd(uint64_t v)   { return (int)((v >> 21) & 0x1FF); }
static inline int idx_pt(uint64_t v)   { return (int)((v >> 12) & 0x1FF); }

static inline uint64_t* entry_table(uint64_t entry) {
    return (uint64_t*)(uintptr_t)(entry & PTE_FRAME_MASK);
}

static uint64_t* alloc_table(void) {
    uint64_t phys = pmm_alloc_page();
    if (phys == 0) return NULL;
    uint64_t* t = (uint64_t*)(uintptr_t)phys;
    memset(t, 0, PAGE_SIZE_4K);
    return t;
}

/* Get (or allocate) the next-level table under table[idx] for KERNEL use */
static uint64_t* kernel_next_level(uint64_t* table, int idx, uint64_t flags) {
    if (!(table[idx] & PTE_PRESENT)) {
        uint64_t* nt = alloc_table();
        if (!nt) return NULL;
        table[idx] = (uint64_t)(uintptr_t)nt | flags;
    }
    return entry_table(table[idx]);
}

/* Identity-map one 2 MiB region into the kernel hierarchy */
static void kernel_map_2m(uint64_t addr, uint64_t extra_flags) {
    uint64_t* pdpt = kernel_next_level(kernel_pml4, idx_pml4(addr),
                                       PTE_PRESENT | PTE_READ_WRITE);
    if (!pdpt) return;
    uint64_t* pd = kernel_next_level(pdpt, idx_pdpt(addr),
                                     PTE_PRESENT | PTE_READ_WRITE);
    if (!pd) return;
    pd[idx_pd(addr)] = (addr & ~(uint64_t)(PAGE_SIZE_2M - 1))
                       | PTE_PRESENT | PTE_READ_WRITE | PTE_PS | extra_flags;
    mapped_2m_pages++;
}

/* ──────── Kernel MMIO mapping (post-init device windows) ──────── */
void paging_kernel_map_mmio(uint64_t phys, uint64_t size) {
    uint64_t base = phys & ~(uint64_t)(PAGE_SIZE_2M - 1);
    uint64_t end  = phys + size;
    for (uint64_t addr = base; addr < end; addr += PAGE_SIZE_2M) {
        kernel_map_2m(addr, PTE_CACHE_DIS);
    }
    /* Full TLB flush: user PML4s share the kernel tables, and the
     * region may previously have been non-present */
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    asm volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
}

/* ──────── Page Fault Handler (ISR 14) ──────── */
static void page_fault_handler(registers_t* regs) {
    uint64_t faulting_addr = read_cr2();
    uint64_t error_code    = regs->err_code;

    /*
     * Ring 3 fault: a game crashed. A console must survive this –
     * kill the offending task, wake its waiters, and reschedule
     * instead of taking the whole system down.
     */
    if ((error_code & 0x04) && current_task != NULL) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("\n[CRASH] Task '");
        terminal_writestring(current_task->name);
        terminal_writestring("' page-faulted at 0x");
        terminal_writehex(faulting_addr);
        terminal_writestring(" (RIP 0x");
        terminal_writehex(regs->eip);
        if (error_code & 0x10)
            terminal_writestring(", NX: tried to execute data");
        terminal_writestring(") - terminated\n");

        current_task->state = TASK_DEAD;
        for (int i = 0; i < num_tasks; i++) {
            if (tasks[i].state == TASK_BLOCKED && tasks[i].wait_pid == current_task->id) {
                tasks[i].state = TASK_READY;
                tasks[i].wait_pid = 0;
            }
        }
        for (;;) {
            schedule();
            asm volatile("sti\nhlt");
        }
    }

    /* Disable interrupts for the panic screen */
    cli();

    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_RED));

    /* Clear first few lines for the panic message */
    for (size_t y = 0; y < 10; y++)
        for (size_t x = 0; x < VGA_WIDTH; x++)
            terminal_putentryat(' ', vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_RED), x, y);

    terminal_row = 0;
    terminal_column = 0;

    terminal_writestring("  *** KERNEL PANIC: PAGE FAULT ***\n\n");
    terminal_writestring("  Faulting address:  0x");
    terminal_writehex(faulting_addr);
    terminal_writestring("\n");

    terminal_writestring("  Error code:        0x");
    terminal_writehex(error_code);
    terminal_writestring("\n");

    terminal_writestring("  Cause: ");
    if (!(error_code & 0x01)) {
        terminal_writestring("Page not present");
    } else {
        terminal_writestring("Protection violation");
    }
    terminal_writestring("\n");

    terminal_writestring("  Access: ");
    if (error_code & 0x02) {
        terminal_writestring("Write");
    } else {
        terminal_writestring("Read");
    }
    terminal_writestring("\n");

    terminal_writestring("  Ring: ");
    if (error_code & 0x04) {
        terminal_writestring("User mode");
    } else {
        terminal_writestring("Kernel mode");
    }
    terminal_writestring("\n");

    terminal_writestring("\n  RIP: 0x");
    terminal_writehex(regs->eip);
    terminal_writestring("\n");

    terminal_writestring("\n  System halted.");

    /* Last act: get the panic text onto the game volume, because the
     * idle-task flusher will never run again. */
    klog_panic_flush();

    for (;;) hlt();
}

/* ──────── Paging initialization ──────── */
void paging_init(void) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
    terminal_writestring("[PAGING] Initializing virtual memory (4-level)...\n");

    /* Step 1: Register the Page Fault handler (ISR 14) */
    register_interrupt_handler(14, page_fault_handler);

    /* Step 1b: Turn on no-execute so user data/stack pages can be NX */
    nx_enable();
    terminal_writestring(pte_nx ? "[PAGING] NX enabled (W^X for user pages)\n"
                                : "[PAGING] NX not supported by this CPU\n");

    /* Step 2: Allocate the kernel PML4 */
    kernel_pml4 = alloc_table();
    if (kernel_pml4 == NULL) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("[PAGING] FATAL: Could not allocate PML4!\n");
        cli();
        for (;;) hlt();
    }

    /* Step 3: Identity-map all physical RAM with 2 MiB pages */
    uint64_t total_phys_bytes = (uint64_t)pmm_get_total_pages() * PAGE_SIZE_4K;
    for (uint64_t addr = 0; addr < total_phys_bytes; addr += PAGE_SIZE_2M) {
        kernel_map_2m(addr, 0);
    }

    /* Step 3b: Identity-map the linear framebuffer (MMIO above RAM,
     * e.g. 0xFD000000 on QEMU stdvga) with write-through caching. */
    if (fb_available()) {
        uint64_t fb_base = fb_phys_addr() & ~(uint64_t)(PAGE_SIZE_2M - 1);
        /* x2: the flip-capable surface is two pages tall */
        uint64_t fb_end  = fb_phys_addr() + (uint64_t)fb_size_bytes() * 2;
        for (uint64_t addr = fb_base; addr < fb_end; addr += PAGE_SIZE_2M) {
            kernel_map_2m(addr, PTE_WRITE_THRU);
        }
    }

    /* Step 4: Load the kernel PML4 (replaces the bootloader's tables) */
    write_cr3((uint64_t)(uintptr_t)kernel_pml4);

    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("[PAGING] Enabled! Identity-mapped ");
    terminal_writedec((uint32_t)(mapped_2m_pages * 2));
    terminal_writestring(" MiB (2 MiB pages, 4-level)\n");
}

/*
 * ──────── Clone kernel PML4 for a new user process ────────
 *
 * Copies all present PML4 entries verbatim – the process initially
 * shares every kernel table (supervisor-only, so Ring 3 cannot touch
 * them). paging_map_page_to() splits off private copies on demand.
 * Returns the PHYSICAL address of the new PML4, or 0 on failure.
 */
uint64_t paging_clone_kernel_pd(void) {
    uint64_t* new_pml4 = alloc_table();
    if (!new_pml4) return 0;

    for (int i = 0; i < ENTRIES_PER_TABLE; i++) {
        new_pml4[i] = kernel_pml4[i];
    }
    return (uint64_t)(uintptr_t)new_pml4;
}

/*
 * Descend one level for a USER mapping. If the entry is missing, allocate
 * a fresh table. If it still points at the kernel's shared table for this
 * slot, allocate a private copy first (so kernel tables stay pristine).
 * The rewritten entry gets the USER bit – ring 3 access requires it at
 * every level; kernel entries *inside* the copied table keep supervisor
 * flags, so games still can't touch kernel memory.
 */
static uint64_t* user_next_level(uint64_t* table, int idx, uint64_t* kernel_table) {
    uint64_t entry  = table[idx];
    uint64_t kentry = kernel_table ? kernel_table[idx] : 0;

    if (!(entry & PTE_PRESENT)) {
        uint64_t* nt = alloc_table();
        if (!nt) return NULL;
        table[idx] = (uint64_t)(uintptr_t)nt | PTE_PRESENT | PTE_READ_WRITE | PTE_USER;
        return nt;
    }

    if ((kentry & PTE_PRESENT) &&
        (entry & PTE_FRAME_MASK) == (kentry & PTE_FRAME_MASK)) {
        /* Shared with the kernel: split off a private copy */
        uint64_t* copy = alloc_table();
        if (!copy) return NULL;
        memcpy(copy, entry_table(kentry), PAGE_SIZE_4K);
        table[idx] = (uint64_t)(uintptr_t)copy | PTE_PRESENT | PTE_READ_WRITE | PTE_USER;
        return copy;
    }

    return entry_table(entry);
}

/* ──────── Map a 4 KiB user page into a process PML4 ──────── */
void paging_map_page_to(uint64_t* pml4, uint64_t vaddr, uint64_t paddr, uint64_t flags) {
    int i4 = idx_pml4(vaddr);
    int i3 = idx_pdpt(vaddr);
    int i2 = idx_pd(vaddr);
    int i1 = idx_pt(vaddr);

    /* Kernel tables along this path (for shared-table detection) */
    uint64_t* kpdpt = (kernel_pml4[i4] & PTE_PRESENT)
                      ? entry_table(kernel_pml4[i4]) : NULL;
    uint64_t* kpd   = (kpdpt && (kpdpt[i3] & PTE_PRESENT))
                      ? entry_table(kpdpt[i3]) : NULL;

    uint64_t* pdpt = user_next_level(pml4, i4, kernel_pml4);
    if (!pdpt) return;
    uint64_t* pd = user_next_level(pdpt, i3, kpdpt);
    if (!pd) return;

    /*
     * PT level: a kernel PD entry here is a 2 MiB identity page (PS set),
     * not a table. Replace it with a fresh, empty 4 KiB page table – the
     * process's user pages OVERLAY the identity map, exactly like the
     * 32-bit design. (This is why exec() must run under the kernel PML4
     * when writing to freshly allocated physical pages.)
     */
    uint64_t pde = pd[i2];
    uint64_t* pt;
    if (!(pde & PTE_PRESENT) || (pde & PTE_PS) ||
        (kpd && (kpd[i2] & PTE_PRESENT) &&
         (pde & PTE_FRAME_MASK) == (kpd[i2] & PTE_FRAME_MASK))) {
        pt = alloc_table();
        if (!pt) return;
        pd[i2] = (uint64_t)(uintptr_t)pt | PTE_PRESENT | PTE_READ_WRITE | PTE_USER;
    } else {
        pt = entry_table(pde);
    }

    pt[i1] = (paddr & PTE_FRAME_MASK) | flags;
}

/* ──────── Dump paging info ──────── */
void paging_dump_info(void) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("Paging Information:\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));

    terminal_writestring("  Kernel PML4:     0x");
    terminal_writehex((uint64_t)(uintptr_t)kernel_pml4);
    terminal_writestring("\n");

    terminal_writestring("  Identity mapped: ");
    terminal_writedec((uint32_t)(mapped_2m_pages * 2));
    terminal_writestring(" MiB (");
    terminal_writedec((uint32_t)mapped_2m_pages);
    terminal_writestring(" x 2 MiB pages)\n");

    terminal_writestring("  Levels:          4 (PML4 > PDPT > PD > PT)\n");
}
