/*
 * ArcadeOS – 32-bit x86 Paging (Virtual Memory)
 *
 * Identity-maps all physical RAM so virtual == physical.
 * Page Directory and Page Tables are allocated from the PMM.
 * ISR 14 is hooked as a Page Fault handler.
 */

#include "paging.h"
#include "pmm.h"
#include "idt.h"
#include "vga.h"
#include "fb.h"
#include "task.h"
#include "scheduler.h"

/*
 * The kernel Page Directory (1024 entries × 4 bytes = 4 KiB).
 * Each entry points to a Page Table (or is empty).
 */
static uint32_t* page_directory = NULL;

/*
 * Keep track of the Page Table pointers so we can access them
 * for mapping/unmapping individual pages later.
 */
static uint32_t* page_tables[PAGES_PER_DIR];

/* Number of 4 MiB regions we identity-mapped */
static uint32_t mapped_regions = 0;

uint32_t* paging_get_kernel_pd(void) {
    return page_directory;
}

/* ──────── CR register helpers ──────── */

/* Read the faulting virtual address from CR2 */
static inline uint32_t read_cr2(void) {
    uint32_t val;
    asm volatile("mov %%cr2, %0" : "=r"(val));
    return val;
}

/* Load a Page Directory physical address into CR3 */
static inline void write_cr3(uint32_t pd_phys) {
    asm volatile("mov %0, %%cr3" :: "r"(pd_phys) : "memory");
}

/* Enable paging by setting bit 31 of CR0 */
static inline void enable_paging_hw(void) {
    uint32_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000;  /* PG bit */
    asm volatile("mov %0, %%cr0" :: "r"(cr0) : "memory");
}

/* ──────── Page Fault Handler (ISR 14) ──────── */
static void page_fault_handler(registers_t* regs) {
    uint32_t faulting_addr = read_cr2();
    uint32_t error_code    = regs->err_code;

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
        terminal_writestring(" (EIP 0x");
        terminal_writehex(regs->eip);
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

    /* Decode the error code bits */
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

    terminal_writestring("\n  EIP: 0x");
    terminal_writehex(regs->eip);
    terminal_writestring("\n");

    terminal_writestring("\n  System halted.");

    /* Halt forever */
    for (;;) hlt();
}

/* ──────── Paging initialization ──────── */
void paging_init(void) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
    terminal_writestring("[PAGING] Initializing virtual memory...\n");

    /* Step 1: Register the Page Fault handler (ISR 14) */
    register_interrupt_handler(14, page_fault_handler);

    /* Step 2: Allocate a page-aligned Page Directory from the PMM */
    uint32_t pd_phys = pmm_alloc_page();
    if (pd_phys == 0) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("[PAGING] FATAL: Could not allocate Page Directory!\n");
        cli();
        for (;;) hlt();
    }
    page_directory = (uint32_t*)pd_phys;

    /* Zero out all 1024 entries (no pages mapped initially) */
    memset(page_directory, 0, PAGE_SIZE_4K);
    memset(page_tables, 0, sizeof(page_tables));

    /*
     * Step 3: Identity-map all physical memory.
     *
     * We must at minimum map:
     *   - 0x00000 – 0xFFFFF  (first 1 MiB: BIOS, VGA buffer at 0xB8000)
     *   - 0x100000+           (kernel code & data, PMM bitmap, heap)
     *
     * For simplicity, we identity-map everything the PMM knows about.
     * Each PDE covers 4 MiB, so we need (total_pages * 4K) / 4M tables.
     */
    uint32_t total_phys_bytes = pmm_get_total_pages() * PAGE_SIZE_4K;
    uint32_t num_pde_needed = total_phys_bytes / PDE_COVER;
    if (total_phys_bytes % PDE_COVER) num_pde_needed++;

    /* Cap at 1024 (full 4 GiB address space) */
    if (num_pde_needed > PAGES_PER_DIR) num_pde_needed = PAGES_PER_DIR;

    for (uint32_t pde = 0; pde < num_pde_needed; pde++) {
        /* Allocate a Page Table (4 KiB, page-aligned) from PMM */
        uint32_t pt_phys = pmm_alloc_page();
        if (pt_phys == 0) {
            terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
            terminal_writestring("[PAGING] FATAL: Out of pages for Page Tables!\n");
            cli();
            for (;;) hlt();
        }

        uint32_t* pt = (uint32_t*)pt_phys;
        page_tables[pde] = pt;

        /* Fill 1024 PTEs: identity map each 4 KiB page in this 4 MiB region */
        uint32_t base_addr = pde * PDE_COVER;
        for (uint32_t pte = 0; pte < PAGES_PER_TABLE; pte++) {
            uint32_t phys = base_addr + (pte * PAGE_SIZE_4K);
            /* REMOVE PTE_USER: Kernel space should NOT be user-accessible */
            pt[pte] = phys | PTE_PRESENT | PTE_READ_WRITE; 
        }

        /* REMOVE PTE_USER from PDE for kernel regions */
        page_directory[pde] = pt_phys | PTE_PRESENT | PTE_READ_WRITE;
    }

    mapped_regions = num_pde_needed;

    /*
     * Step 3b: Identity-map the linear framebuffer.
     * The LFB lives in MMIO space above physical RAM (e.g. 0xFD000000
     * on QEMU stdvga), so the loop above never covered it. Without this
     * mapping the first console write after enable_paging_hw() would
     * page-fault.
     */
    if (fb_available()) {
        uint32_t fb_base  = fb_phys_addr() & PTE_FRAME_MASK;
        uint32_t fb_pages = fb_size_bytes() / PAGE_SIZE_4K;
        for (uint32_t i = 0; i < fb_pages; i++) {
            uint32_t addr = fb_base + i * PAGE_SIZE_4K;
            paging_map_page(addr, addr, PTE_PRESENT | PTE_READ_WRITE | PTE_WRITE_THRU);
        }
    }

    /* Step 4: Load the Page Directory into CR3 and enable paging */
    write_cr3(pd_phys);
    enable_paging_hw();

    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("[PAGING] Enabled! Identity-mapped ");
    terminal_writedec(mapped_regions * 4);
    terminal_writestring(" MiB (");
    terminal_writedec(mapped_regions);
    terminal_writestring(" page tables)\n");
}

/*
 * ──────── Clone kernel PD for a new user process ────────
 *
 * Creates a fresh, empty Page Directory and copies all currently
 * mapped KERNEL PDE entries verbatim (shared page tables, no user bit).
 * User-space regions (above 0xC0000000 is the conventional boundary,
 * but we are identity-mapped so we copy ALL kernel entries and let the
 * caller overlay user pages on top with paging_map_page_to()).
 *
 * Returns the PHYSICAL address of the new PD (for loading into CR3).
 * Returns 0 on allocation failure.
 */
uint32_t paging_clone_kernel_pd(void) {
    uint32_t new_pd_phys = pmm_alloc_page();
    if (new_pd_phys == 0) return 0;

    uint32_t* new_pd = (uint32_t*)new_pd_phys;

    /* Zero out all 1024 entries first */
    memset(new_pd, 0, PAGE_SIZE_4K);

    /*
     * Copy kernel PDEs.  We share the SAME physical page tables –
     * they are supervisor-only (no PTE_USER on the PDEs), so they
     * cannot be accessed from Ring 3.  Any write to kernel PDEs by
     * the child would cause a GP fault rather than silently corrupting
     * kernel state.
     */
    for (uint32_t i = 0; i < PAGES_PER_DIR; i++) {
        if (page_directory[i] & PTE_PRESENT) {
            /* Share the kernel page table; keep the same PDE flags */
            new_pd[i] = page_directory[i];
        }
    }

    return new_pd_phys;
}

/* ──────── Map a single page ──────── */
void paging_map_page(uint32_t virtual_addr, uint32_t physical_addr, uint32_t flags) {
    uint32_t pde_index = virtual_addr / PDE_COVER;
    uint32_t pte_index = (virtual_addr % PDE_COVER) / PAGE_SIZE_4K;

    /* If no page table exists for this PDE, allocate one */
    if (page_tables[pde_index] == NULL) {
        uint32_t pt_phys = pmm_alloc_page();
        if (pt_phys == 0) return;

        uint32_t* pt = (uint32_t*)pt_phys;
        memset(pt, 0, PAGE_SIZE_4K);
        page_tables[pde_index] = pt;
        page_directory[pde_index] = pt_phys | PTE_PRESENT | PTE_READ_WRITE | PTE_USER;
    }

    /* Set the PTE */
    page_tables[pde_index][pte_index] = (physical_addr & PTE_FRAME_MASK) | flags;

    /* Flush the TLB for this address */
    asm volatile("invlpg (%0)" :: "r"(virtual_addr) : "memory");
}

void paging_map_page_to(uint32_t* pd, uint32_t vaddr, uint32_t paddr, uint32_t flags) {
    uint32_t pde_idx = vaddr >> 22;
    uint32_t pte_idx = (vaddr >> 12) & 0x3FF;
    
    /* If no page table exists, or it belongs to the kernel, allocate a private one */
    if (!(pd[pde_idx] & PTE_PRESENT) || (pd[pde_idx] & PTE_FRAME_MASK) == (page_directory[pde_idx] & PTE_FRAME_MASK)) {
        uint32_t pt_phys = pmm_alloc_page();
        uint32_t* pt = (uint32_t*)pt_phys;
        memset(pt, 0, PAGE_SIZE_4K);
        /* Set PDE with USER bit so apps can traverse it */
        pd[pde_idx] = pt_phys | PTE_PRESENT | PTE_READ_WRITE | PTE_USER;
    }
    
    uint32_t* pt = (uint32_t*)(pd[pde_idx] & PTE_FRAME_MASK);
    pt[pte_idx] = (paddr & PTE_FRAME_MASK) | flags;
}

/* ──────── Unmap a single page ──────── */
void paging_unmap_page(uint32_t virtual_addr) {
    uint32_t pde_index = virtual_addr / PDE_COVER;
    uint32_t pte_index = (virtual_addr % PDE_COVER) / PAGE_SIZE_4K;

    if (page_tables[pde_index] == NULL) return;

    /* Clear the PTE */
    page_tables[pde_index][pte_index] = 0;

    /* Flush the TLB for this address */
    asm volatile("invlpg (%0)" :: "r"(virtual_addr) : "memory");
}

/* ──────── Dump paging info ──────── */
void paging_dump_info(void) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("Paging Information:\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));

    terminal_writestring("  Page Directory:  0x");
    terminal_writehex((uint32_t)page_directory);
    terminal_writestring("\n");

    terminal_writestring("  Identity mapped: ");
    terminal_writedec(mapped_regions * 4);
    terminal_writestring(" MiB (");
    terminal_writedec(mapped_regions);
    terminal_writestring(" page tables)\n");

    terminal_writestring("  Page size:       4 KiB\n");
    terminal_writestring("  PDE coverage:    4 MiB each\n");

    /* Count total mapped pages */
    uint32_t mapped_pages = 0;
    for (uint32_t pde = 0; pde < PAGES_PER_DIR; pde++) {
        if (page_tables[pde] != NULL) {
            for (uint32_t pte = 0; pte < PAGES_PER_TABLE; pte++) {
                if (page_tables[pde][pte] & PTE_PRESENT) {
                    mapped_pages++;
                }
            }
        }
    }

    terminal_writestring("  Mapped pages:    ");
    terminal_writedec(mapped_pages);
    terminal_writestring(" (");
    terminal_writedec(mapped_pages * 4);
    terminal_writestring(" KiB)\n");
}
