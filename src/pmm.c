/*
 * ArcadeOS – Bitmap-based Physical Memory Manager
 *
 * Each bit in the bitmap represents a 4 KiB page.
 * Bit = 1 means the page is USED, bit = 0 means FREE.
 *
 * The bitmap itself is placed right after kernel_end.
 * The heap then starts after the bitmap.
 */

#include "pmm.h"
#include "vga.h"

/* Linker-provided symbol marking the end of the kernel BSS */
extern uint8_t kernel_end;

/* Bitmap storage */
static uint8_t*  pmm_bitmap     = NULL;
static uint32_t  pmm_bitmap_size = 0;   /* Number of bytes in the bitmap */
static uint32_t  total_pages    = 0;
static uint32_t  used_pages     = 0;

/* ──────── Internal helpers ──────── */
static void pmm_set_page(uint32_t page) {
    pmm_bitmap[page / 8] |= (uint8_t)(1 << (page % 8));
}

static void pmm_clear_page(uint32_t page) {
    pmm_bitmap[page / 8] &= (uint8_t)(~(1 << (page % 8)));
}

static int pmm_test_page(uint32_t page) {
    return pmm_bitmap[page / 8] & (1 << (page % 8));
}

/* Mark a range of pages as used */
static void pmm_mark_region_used(uint32_t base, uint32_t length) {
    uint32_t start_page = base / PAGE_SIZE;
    uint32_t page_count = length / PAGE_SIZE;
    if (length % PAGE_SIZE) page_count++;

    for (uint32_t i = 0; i < page_count && (start_page + i) < total_pages; i++) {
        if (!pmm_test_page(start_page + i)) {
            pmm_set_page(start_page + i);
            used_pages++;
        }
    }
}

/* Mark a range of pages as free */
static void pmm_mark_region_free(uint32_t base, uint32_t length) {
    uint32_t start_page = base / PAGE_SIZE;
    uint32_t page_count = length / PAGE_SIZE;

    for (uint32_t i = 0; i < page_count && (start_page + i) < total_pages; i++) {
        if (pmm_test_page(start_page + i)) {
            pmm_clear_page(start_page + i);
            used_pages--;
        }
    }
}

/* ──────── Public API ──────── */
void pmm_init(multiboot_info_t* mboot_info) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
    terminal_writestring("[PMM] Initializing physical memory manager...\n");

    /*
     * Step 1: Determine total physical memory.
     * Use mem_upper from multiboot (KiB above 1 MiB).
     * Total RAM = 1 MiB + mem_upper KiB.
     */
    uint32_t total_memory_kb = 0;

    if (mboot_info->flags & MULTIBOOT_FLAG_MEM) {
        total_memory_kb = 1024 + mboot_info->mem_upper;  /* 1 MiB + upper */
        terminal_writestring("[PMM] Memory: ");
        terminal_writedec(total_memory_kb / 1024);
        terminal_writestring(" MiB (");
        terminal_writedec(total_memory_kb);
        terminal_writestring(" KiB)\n");
    } else {
        /* Fallback: assume 16 MiB */
        total_memory_kb = 16 * 1024;
        terminal_writestring("[PMM] No memory info from GRUB, assuming 16 MiB\n");
    }

    uint32_t total_memory_bytes = total_memory_kb * 1024;
    total_pages = total_memory_bytes / PAGE_SIZE;

    /*
     * Step 2: Place the bitmap right after kernel_end.
     * Each bit = 1 page, so bitmap_size = total_pages / 8.
     */
    pmm_bitmap_size = total_pages / 8;
    if (total_pages % 8) pmm_bitmap_size++;

    pmm_bitmap = (uint8_t*)&kernel_end;
    /* Align to 16 bytes */
    pmm_bitmap = (uint8_t*)(((uintptr_t)pmm_bitmap + 15) & ~(uintptr_t)15);

    /* Step 3: Start by marking ALL pages as used */
    for (uint32_t i = 0; i < pmm_bitmap_size; i++)
        pmm_bitmap[i] = 0xFF;
    used_pages = total_pages;

    /*
     * Step 4: Walk the multiboot memory map and mark available regions as free.
     */
    if (mboot_info->flags & MULTIBOOT_FLAG_MMAP) {
        terminal_writestring("[PMM] Parsing memory map (");
        terminal_writedec(mboot_info->mmap_length);
        terminal_writestring(" bytes)...\n");

        uintptr_t mmap_addr = mboot_info->mmap_addr;
        uintptr_t mmap_end  = mmap_addr + mboot_info->mmap_length;

        while (mmap_addr < mmap_end) {
            multiboot_mmap_entry_t* entry = (multiboot_mmap_entry_t*)mmap_addr;

            /* Only care about 32-bit addresses (base_high == 0) */
            if (entry->base_high == 0 && entry->type == MULTIBOOT_MMAP_AVAILABLE) {
                uint32_t region_base   = entry->base_low;
                uint32_t region_length = entry->length_low;

                /* Don't free anything below 1 MiB (BIOS/VGA/kernel load area) */
                if (region_base < 0x100000) {
                    if (region_base + region_length > 0x100000) {
                        /* Region spans into usable area */
                        region_length -= (0x100000 - region_base);
                        region_base = 0x100000;
                    } else {
                        /* Entirely below 1 MiB, skip */
                        mmap_addr += entry->size + 4;
                        continue;
                    }
                }

                pmm_mark_region_free(region_base, region_length);
            }

            mmap_addr += entry->size + 4;
        }
    } else {
        /* Fallback: assume everything from 1 MiB to total_memory is available */
        terminal_writestring("[PMM] No memory map, using fallback\n");
        pmm_mark_region_free(0x100000, total_memory_bytes - 0x100000);
    }

    /*
     * Step 5: Re-mark the kernel + bitmap region as used.
     * The kernel is loaded at 1 MiB. The bitmap comes after kernel_end.
     * Protect from 1 MiB to (bitmap_end + some padding).
     */
    uint32_t kernel_start_addr = 0x100000;  /* 1 MiB – where GRUB loads us */
    uint32_t bitmap_end = (uint32_t)(uintptr_t)pmm_bitmap + pmm_bitmap_size;
    /* Add 4 KiB safety margin */
    uint32_t protected_end = bitmap_end + PAGE_SIZE;
    uint32_t protected_size = protected_end - kernel_start_addr;

    pmm_mark_region_used(kernel_start_addr, protected_size);

    /* Also protect page 0 (null pointer dereference catcher) */
    if (!pmm_test_page(0)) {
        pmm_set_page(0);
        used_pages++;
    }

    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("[PMM] Ready: ");
    terminal_writedec(pmm_get_free_pages());
    terminal_writestring(" free pages (");
    terminal_writedec(pmm_get_free_pages() * 4);
    terminal_writestring(" KiB)\n");
}

uint64_t pmm_alloc_page(void) {
    for (uint32_t i = 0; i < total_pages; i++) {
        if (!pmm_test_page(i)) {
            pmm_set_page(i);
            used_pages++;
            return (uint64_t)i * PAGE_SIZE;
        }
    }
    return 0;  /* Out of physical memory */
}

void pmm_free_page(uint64_t addr) {
    uint64_t page = addr / PAGE_SIZE;
    if (page >= total_pages) return;
    if (page == 0) return;  /* Never free page 0 */

    if (pmm_test_page(page)) {
        pmm_clear_page(page);
        used_pages--;
    }
}

uint32_t pmm_get_total_pages(void) { return total_pages; }
uint32_t pmm_get_used_pages(void)  { return used_pages; }
uint32_t pmm_get_free_pages(void)  { return total_pages - used_pages; }

void pmm_dump_info(void) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("Physical Memory Manager:\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));

    terminal_writestring("  Total RAM:     ");
    terminal_writedec(total_pages * 4);
    terminal_writestring(" KiB (");
    terminal_writedec(total_pages * 4 / 1024);
    terminal_writestring(" MiB)\n");

    terminal_writestring("  Total pages:   ");
    terminal_writedec(total_pages);
    terminal_writestring("\n");

    terminal_writestring("  Used pages:    ");
    terminal_writedec(used_pages);
    terminal_writestring(" (");
    terminal_writedec(used_pages * 4);
    terminal_writestring(" KiB)\n");

    terminal_writestring("  Free pages:    ");
    terminal_writedec(pmm_get_free_pages());
    terminal_writestring(" (");
    terminal_writedec(pmm_get_free_pages() * 4);
    terminal_writestring(" KiB)\n");

    terminal_writestring("  Bitmap at:     0x");
    terminal_writehex((uint32_t)(uintptr_t)pmm_bitmap);
    terminal_writestring("\n");

    terminal_writestring("  Bitmap size:   ");
    terminal_writedec(pmm_bitmap_size);
    terminal_writestring(" bytes\n");
}

/*
 * ──────── Contiguous multi-page allocation ────────
 *
 * Needed for physically-contiguous buffers such as the graphics
 * back buffer and (later) USB controller DMA schedules.
 * Scans the bitmap for a run of 'count' free pages, marks them used,
 * and returns the physical address of the first page (0 on failure).
 */
uint64_t pmm_alloc_pages(uint32_t count) {
    if (count == 0) return 0;
    if (count == 1) return pmm_alloc_page();

    uint32_t run_start = 0;
    uint32_t run_len   = 0;

    for (uint32_t i = 0; i < total_pages; i++) {
        if (!pmm_test_page(i)) {
            if (run_len == 0) run_start = i;
            run_len++;
            if (run_len == count) {
                for (uint32_t p = run_start; p < run_start + count; p++) {
                    pmm_set_page(p);
                    used_pages++;
                }
                return (uint64_t)run_start * PAGE_SIZE;
            }
        } else {
            run_len = 0;
        }
    }
    return 0;   /* No contiguous run large enough */
}

void pmm_free_pages(uint64_t addr, uint32_t count) {
    for (uint32_t i = 0; i < count; i++)
        pmm_free_page(addr + (uint64_t)i * PAGE_SIZE);
}
