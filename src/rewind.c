/*
 * ArcadeOS – kernel-level universal rewind (see rewind.h)
 *
 * Mechanics:
 *   - rewind_init() grabs REWIND_SLOTS contiguous 4 MiB buffers from
 *     the PMM at boot (24 MiB total on the default config).
 *   - When a task presents its first frame, its writable pages are
 *     enumerated once (they never change after load: games have no
 *     heap and no dynamic mapping).
 *   - Every SNAP_INTERVAL presents, all those pages are copied into
 *     the next ring slot — under the kernel CR3, because both the
 *     game frames and the slot buffers are raw physical pages.
 *   - The SELECT+L1 chord (edge-detected in rewind_filter_pad) pops
 *     the newest snapshot: pages are copied back and the game simply
 *     returns from SYS_GFX_PRESENT into its restored past. Each press
 *     steps another ~2 s further back.
 */

#include "rewind.h"
#include "paging.h"
#include "pmm.h"
#include "task.h"
#include "scheduler.h"
#include "vga.h"
#include "audio.h"
#include "gamepad.h"
#include "clock.h"

#define REWIND_SLOTS      6
#define REWIND_MAX_PAGES  1024          /* 4 MiB per slot */
#define SNAP_INTERVAL     120           /* Presents between snapshots (~2 s) */

#define CHORD (PAD_BTN_SELECT | PAD_BTN_L1)

static uint64_t slots[REWIND_SLOTS];    /* Physical buffer per slot */
static int      enabled = 0;

/* Snapshot ring: stack semantics on top of a ring buffer */
static int ring_base  = 0;              /* Index of the oldest snapshot */
static int ring_count = 0;

/* The owning task + its page list (fixed once per exec) */
static uint32_t    owner_id = 0;
static user_page_t pages[REWIND_MAX_PAGES];
static int         npages = 0;

static uint32_t presents_since_snap = 0;
static int      chord_was_down = 0;
static volatile int rewind_requested = 0;

void rewind_init(void) {
    for (int i = 0; i < REWIND_SLOTS; i++) {
        slots[i] = pmm_alloc_pages(REWIND_MAX_PAGES);
        if (!slots[i]) {
            terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK));
            terminal_writestring("[RWD] Not enough RAM - rewind disabled\n");
            return;
        }
    }
    enabled = 1;
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("[RWD] Universal rewind ready (");
    terminal_writedec(REWIND_SLOTS);
    terminal_writestring(" snapshots, SELECT+L1 to rewind)\n");
}

/* Copy game pages ↔ slot buffer. Direction 0 = snapshot, 1 = restore.
 * Under the kernel CR3: both sides are physical frames. */
static void slot_copy(int slot_idx, int restore) {
    cr3_borrow_t b = paging_borrow_kernel_cr3();
    uint8_t* buf = (uint8_t*)(uintptr_t)slots[slot_idx];
    for (int i = 0; i < npages; i++) {
        void* frame = (void*)(uintptr_t)pages[i].phys;
        if (restore)
            memcpy(frame, buf + (uint64_t)i * 4096, 4096);
        else
            memcpy(buf + (uint64_t)i * 4096, frame, 4096);
    }
    paging_return_cr3(b);
}

static void take_snapshot(void) {
    int slot = (ring_base + ring_count) % REWIND_SLOTS;
    if (ring_count == REWIND_SLOTS) {
        /* Ring full: overwrite the oldest */
        slot = ring_base;
        ring_base = (ring_base + 1) % REWIND_SLOTS;
        ring_count--;
    }
    slot_copy(slot, 0);
    ring_count++;
}

static void do_rewind(void) {
    if (ring_count == 0) return;

    /* Pop the newest snapshot and restore it */
    ring_count--;
    int slot = (ring_base + ring_count) % REWIND_SLOTS;
    slot_copy(slot, 1);

    /* Audible cue on the system voice: descending "whoosh" */
    audio_tone_voice(MIX_VOICES - 1, 1200, 80, 160);

    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("[RWD] Rewound (");
    terminal_writedec((uint32_t)ring_count);
    terminal_writestring(" snapshots left)\n");

    presents_since_snap = 0;
}

void rewind_on_present(void) {
    if (!enabled || !current_task) return;

    /* New process on the screen: rebuild the page list, drop history */
    if (current_task->id != owner_id) {
        owner_id = current_task->id;
        npages   = paging_collect_user_rw(pages, REWIND_MAX_PAGES);
        ring_base = ring_count = 0;
        presents_since_snap = 0;
        rewind_requested = 0;
        /* Snapshot #0 right away: rewinding to "just started" works */
        take_snapshot();
        return;
    }

    if (rewind_requested) {
        rewind_requested = 0;
        do_rewind();
        return;
    }

    if (++presents_since_snap >= SNAP_INTERVAL) {
        presents_since_snap = 0;
        take_snapshot();
    }
}

int rewind_filter_pad(int index, pad_state_t* st) {
    static uint32_t cooldown_until = 0;
    static uint32_t grace_until    = 0;
    static int      select_graced  = 0;

    if (!enabled || index != 0) return 0;

    int chord = (st->buttons & CHORD) == CHORD;
    if (chord) {
        if (!chord_was_down)
            rewind_requested = 1;        /* Edge: one step per press */
        chord_was_down = 1;
        select_graced  = 0;

        /* System combo: the game must not see SELECT (quit) or L1 */
        st->buttons = 0;
        st->lx = st->ly = st->rx = st->ry = 0;
        cooldown_until = system_ticks + 400;
        return 1;
    }
    chord_was_down = 0;

    /* After release: the kernel latches press edges for slow game
     * loops, so a chord key can replay as a lone SELECT one poll
     * later — most games treat that as QUIT. Keep swallowing. */
    if (system_ticks < cooldown_until) {
        st->buttons &= (uint16_t)~CHORD;
        return 0;
    }

    /* Before the chord registers: the two keys arrive microseconds
     * apart, and a poll can land between them — a lone SELECT again.
     * Withhold SELECT briefly; if L1 never joins, hand it to the game
     * late (a real quit press just gets a 150 ms delay). */
    if (st->buttons & PAD_BTN_SELECT) {
        if (!select_graced) {
            select_graced = 1;
            grace_until   = system_ticks + 150;
        }
        if (system_ticks < grace_until) {
            st->buttons &= (uint16_t)~PAD_BTN_SELECT;
            /* This poll consumed the press-edge latch — re-arm it, or
             * a tapped SELECT (quit) vanishes instead of arriving
             * after the grace window. */
            gamepad_relatch(index, PAD_BTN_SELECT);
        }
    } else {
        select_graced = 0;
    }
    return 0;
}
