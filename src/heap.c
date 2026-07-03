/*
 * ArcadeOS – Free-List Heap Allocator (backed by PMM pages)
 *
 * On init, requests HEAP_INITIAL_PAGES contiguous pages from the PMM
 * and initializes a free-list block allocator within that region.
 */

#include "heap.h"
#include "pmm.h"
#include "vga.h"

/* Block header prepended to every allocation */
typedef struct block_header {
    size_t                size;   /* Usable size (excluding header) */
    int                   free;
    struct block_header*  next;
} block_header_t;

#define HEADER_SIZE sizeof(block_header_t)
#define BLOCK_ALIGN 16   /* Keep allocations 16-byte aligned */

static block_header_t* heap_start = NULL;
static size_t          heap_total = 0;

/* ──────── Align up to BLOCK_ALIGN ──────── */
static size_t align_up(size_t n) {
    return (n + BLOCK_ALIGN - 1) & ~(BLOCK_ALIGN - 1);
}

/* ──────── Public API ──────── */
void heap_init(void) {
    /*
     * Allocate HEAP_INITIAL_PAGES pages from the PMM.
     * We need a contiguous virtual block, but since we have
     * no paging enabled (identity mapped), we find the first page
     * and try to allocate consecutive ones.
     */
    uint32_t first_page = pmm_alloc_page();
    if (first_page == 0) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("[HEAP] FATAL: Could not allocate initial page!\n");
        return;
    }

    /* Allocate remaining pages (they should be consecutive in a fresh PMM) */
    uint32_t pages_allocated = 1;
    for (uint32_t i = 1; i < HEAP_INITIAL_PAGES; i++) {
        uint32_t page = pmm_alloc_page();
        if (page == 0) break;
        /* Check if this page is contiguous with our block */
        if (page == first_page + (pages_allocated * PAGE_SIZE)) {
            pages_allocated++;
        } else {
            /* Non-contiguous — free it and stop. Use what we have. */
            pmm_free_page(page);
            break;
        }
    }

    heap_total = pages_allocated * PAGE_SIZE;
    heap_start = (block_header_t*)first_page;

    /* Single free block spanning the entire heap */
    heap_start->size = heap_total - HEADER_SIZE;
    heap_start->free = 1;
    heap_start->next = NULL;

    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("[HEAP] Initialized: ");
    terminal_writedec((uint32_t)(heap_total / 1024));
    terminal_writestring(" KiB at 0x");
    terminal_writehex(first_page);
    terminal_writestring(" (");
    terminal_writedec(pages_allocated);
    terminal_writestring(" pages)\n");
}

void* kmalloc(size_t size) {
    if (size == 0) return NULL;
    size = align_up(size);

    block_header_t* cur = heap_start;

    /* First-fit search */
    while (cur != NULL) {
        if (cur->free && cur->size >= size) {
            /* Split the block if there's enough room for another header + data */
            if (cur->size > size + HEADER_SIZE + BLOCK_ALIGN) {
                block_header_t* new_block = (block_header_t*)((uint8_t*)cur + HEADER_SIZE + size);
                new_block->size = cur->size - size - HEADER_SIZE;
                new_block->free = 1;
                new_block->next = cur->next;

                cur->size = size;
                cur->next = new_block;
            }
            cur->free = 0;
            return (void*)((uint8_t*)cur + HEADER_SIZE);
        }
        cur = cur->next;
    }

    return NULL;  /* Out of heap memory */
}

void kfree(void* ptr) {
    if (ptr == NULL) return;

    block_header_t* header = (block_header_t*)((uint8_t*)ptr - HEADER_SIZE);
    header->free = 1;

    /* Coalesce adjacent free blocks */
    block_header_t* cur = heap_start;
    while (cur != NULL) {
        if (cur->free && cur->next != NULL && cur->next->free) {
            cur->size += HEADER_SIZE + cur->next->size;
            cur->next = cur->next->next;
            /* Don't advance – check coalesce again from same position */
        } else {
            cur = cur->next;
        }
    }
}

/* Helper to show how many PMM pages the heap is using */
static uint32_t pages_used_count(void) {
    return (uint32_t)(heap_total / PAGE_SIZE);
}

void heap_dump_info(void) {
    size_t total_free = 0;
    size_t total_used = 0;
    uint32_t block_count = 0;
    uint32_t free_count = 0;

    block_header_t* cur = heap_start;
    while (cur != NULL) {
        block_count++;
        if (cur->free) {
            free_count++;
            total_free += cur->size;
        } else {
            total_used += cur->size;
        }
        cur = cur->next;
    }

    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("Heap Information:\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));

    terminal_writestring("  Start address: 0x");
    terminal_writehex((uint32_t)heap_start);
    terminal_writestring("\n");

    terminal_writestring("  Total size:    ");
    terminal_writedec((uint32_t)(heap_total / 1024));
    terminal_writestring(" KiB (");
    terminal_writedec(pages_used_count());
    terminal_writestring(" PMM pages)\n");

    terminal_writestring("  Used:          ");
    terminal_writedec((uint32_t)(total_used / 1024));
    terminal_writestring(" KiB (");
    terminal_writedec((uint32_t)total_used);
    terminal_writestring(" bytes)\n");

    terminal_writestring("  Free:          ");
    terminal_writedec((uint32_t)(total_free / 1024));
    terminal_writestring(" KiB (");
    terminal_writedec((uint32_t)total_free);
    terminal_writestring(" bytes)\n");

    terminal_writestring("  Blocks:        ");
    terminal_writedec(block_count);
    terminal_writestring(" (");
    terminal_writedec(free_count);
    terminal_writestring(" free)\n");
}
