/*
 * ArcadeOS – kernel-level universal rewind, v2 (see rewind.h)
 *
 * v1 gave every game coarse save-states: full copies of its writable
 * pages every ~2 s in a ring, restored at SYS_GFX_PRESENT boundaries.
 *
 * v2 adds frame-exact scrubbing on top. The insight: an SDK game is a
 * pure function of its memory plus what its syscalls return. So the
 * kernel RECORDS, per presented frame, everything the game observed —
 * both pad states and the tick value — and can then reach ANY frame by
 * restoring the nearest snapshot at or before it and letting the game
 * itself re-execute forward while the kernel feeds it the recorded
 * observations (msleep skipped, blits suppressed, sound/saves muted).
 * The game becomes its own replay engine.
 *
 * Scrubbing UX: HOLD SELECT+L1 (Tab+Q) and time runs visibly backward
 * in SCRUB_STEP_FRAMES hops (each hop = restore + fast replay); release
 * to resume playing from the frame on screen. A virtual clock offset
 * keeps SYS_TICKS continuous across the cut, so games that stash
 * absolute tick values (spawn timers etc.) never see time jump.
 */

#include "rewind.h"
#include "paging.h"
#include "pmm.h"
#include "task.h"
#include "scheduler.h"
#include "vga.h"
#include "fb.h"
#include "usb.h"
#include "audio.h"
#include "gamepad.h"
#include "clock.h"
#include "sysmenu.h"
#include "fat32.h"

#define REWIND_SLOTS      6
#define REWIND_MAX_PAGES  1024          /* 4 MiB per slot */
#define SNAP_INTERVAL     120           /* Presents between snapshots (~2 s) */

#define CHORD      (PAD_BTN_SELECT | PAD_BTN_L1)     /* Hold to scrub */
#define MENU_CHORD (PAD_BTN_SELECT | PAD_BTN_START)  /* System menu */

/* Scrub pacing: one hop of SCRUB_STEP_FRAMES every SCRUB_STEP_MS of
 * real time. 8 frames / 66 ms ≈ 2x rewind speed. */
#define SCRUB_STEP_FRAMES 8
#define SCRUB_STEP_MS     66

/* Frame observation log. Must span the whole snapshot ring:
 * 6 slots x 120 presents = 720 < 1024. */
#define RECORDS 1024
typedef struct {
    pad_state_t pad[2];      /* What SYS_PAD_READ returned (post-filter) */
    uint32_t    vticks;      /* Virtual SYS_TICKS value at this present */
} frame_rec_t;

static uint64_t slots[REWIND_SLOTS];    /* Physical buffer per slot */
static int      enabled = 0;

/* Snapshot ring: stack semantics on top of a ring buffer */
static int      ring_base  = 0;         /* Index of the oldest snapshot */
static int      ring_count = 0;
static uint32_t slot_time[REWIND_SLOTS];   /* Real ticks (menu ages) */
static uint32_t slot_thumb[REWIND_SLOTS][REWIND_THUMB_W * REWIND_THUMB_H];
static uint32_t slot_vticks[REWIND_SLOTS]; /* Virtual ticks (continuity) */
static uint32_t slot_frame[REWIND_SLOTS];  /* Present counter at snapshot */

/* The owning task + its page list (fixed once per exec) */
static uint32_t    owner_id = 0;
static user_page_t pages[REWIND_MAX_PAGES];
static int         npages = 0;

static frame_rec_t recs[RECORDS];

/* ──────── Attract mode (self-playing demos) ──────────────────────────
 *
 * The console demos itself when idle. During normal play the kernel
 * captures the first DEMO_FRAMES of {pad, vticks} plus the seed tick the
 * game read in arcade_init; on exit that becomes <GAME>.DEM on the
 * volume. Attract replays it into a fresh instance of the same game,
 * feeding the recorded inputs AND ticks — because SDK games are
 * deterministic (owned PRNG seeded from ticks()), reproducing the seed
 * tick + per-frame inputs re-runs the recorded session EXACTLY, with
 * the display on and at real-time speed. Any real button ends it.
 */
#define DEMO_FRAMES 1024
#define DEMO_MIN    180              /* Need ~3 s to bother saving */
#define DEMO_MAGIC  0x44454D31u      /* 'DEM1' */

static frame_rec_t demo_cap[DEMO_FRAMES];   /* Captured during live play */
static int         demo_cap_n;
static int         cap_active;
static uint32_t    demo_seed;               /* Seed tick this game read */
static uint32_t    g_last_tick;             /* Last SYS_TICKS returned live */

static frame_rec_t demo_play[DEMO_FRAMES];  /* Loaded demo being replayed */
static int         demo_play_n;
static uint32_t    attract_seed;
static char        attract_game[24];        /* Basename of the attract game */
static int         attract_active;
static int         attract_play;            /* 0 = pre-first-present, 1 = playing */
static int         attract_cursor;
static volatile int attract_cancel;
static uint32_t    attract_owner;
static uint32_t    attract_deadline;

typedef struct { uint32_t magic, seed, nframes; } demo_hdr_t;
static uint8_t demo_io[sizeof(demo_hdr_t) + DEMO_FRAMES * sizeof(frame_rec_t)];

static const char* rw_basename(const char* p) {
    const char* b = p;
    for (const char* q = p; *q; q++) if (*q == '/') b = q + 1;
    return b;
}

/* "PONG.ELF" -> "PONG.DEM" (in place-ish, into out[16]) */
static void demo_name(const char* elfbase, char* out) {
    int i = 0;
    for (; elfbase[i] && elfbase[i] != '.' && i < 8; i++) out[i] = elfbase[i];
    out[i++] = '.'; out[i++] = 'D'; out[i++] = 'E'; out[i++] = 'M';
    out[i] = '\0';
}

static int attract_is_current(void) {
    return attract_active && current_task &&
           strcmp(rw_basename(current_task->name), attract_game) == 0;
}

/* End the attract session and drop the game back to the launcher. */
static void attract_end(void) {
    attract_active = 0;
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("[ATTRACT] demo ended\n");
    if (!current_task) return;
    current_task->state = TASK_DEAD;
    for (int i = 0; i < num_tasks; i++)
        if (tasks[i].state == TASK_BLOCKED && tasks[i].wait_pid == current_task->id) {
            tasks[i].state = TASK_READY;
            tasks[i].wait_pid = 0;
        }
    for (;;) { schedule(); asm volatile("sti\nhlt"); }
}

#define MODE_LIVE   0
#define MODE_REPLAY 1
static int      mode = MODE_LIVE;
static uint32_t present_counter = 0;    /* Frames presented on this timeline */
static uint32_t replay_cursor   = 0;    /* Frame the replaying game is at */
static uint32_t replay_until    = 0;    /* Frame to stop and show */
static int      just_restored   = 0;    /* Next present shows the snapshot */
static int      arrived         = 0;    /* This present reached the target */
static int      scrubbing       = 0;    /* Chord session in progress */
static uint32_t scrub_anchor    = 0;    /* Live frame where the scrub began */
static int      blit_now        = 1;    /* Should this present hit the screen? */

static uint32_t tick_offset = 0;        /* Virtual clock: vticks = real - offset */

static uint32_t presents_since_snap = 0;
static volatile int rewind_requested = 0;   /* System-menu restore */
static int      rewind_pop_n = 1;

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
    terminal_writestring(" snapshots, hold SELECT+L1 to scrub)\n");
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

/* Downsample the displayed frame into the slot's thumbnail */
static void thumb_capture(int slot) {
    if (!fb_available()) return;
    const uint32_t* fb = fb_ptr();
    uint32_t pitch = fb_pitch() / 4;
    uint32_t sx = (fb_width()  << 8) / REWIND_THUMB_W;   /* 24.8 step */
    uint32_t sy = (fb_height() << 8) / REWIND_THUMB_H;
    for (int ty = 0; ty < REWIND_THUMB_H; ty++) {
        uint32_t srcy = ((uint32_t)ty * sy) >> 8;
        for (int tx = 0; tx < REWIND_THUMB_W; tx++)
            slot_thumb[slot][ty * REWIND_THUMB_W + tx] =
                fb[srcy * pitch + (((uint32_t)tx * sx) >> 8)];
    }
}

const uint32_t* rewind_snapshot_thumb(int i) {
    if (!enabled || i < 0 || i >= ring_count) return 0;
    int slot = (ring_base + ring_count - 1 - i) % REWIND_SLOTS;
    return slot_thumb[slot];
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
    thumb_capture(slot);
    slot_time[slot]   = system_ticks;
    slot_vticks[slot] = system_ticks - tick_offset;
    slot_frame[slot]  = present_counter;
    ring_count++;
}

int rewind_snapshot_count(void) {
    return enabled ? ring_count : 0;
}

uint32_t rewind_snapshot_age_ms(int i) {
    if (!enabled || i < 0 || i >= ring_count) return 0;
    int slot = (ring_base + ring_count - 1 - i) % REWIND_SLOTS;
    return system_ticks - slot_time[slot];
}

void rewind_request_restore(int i) {
    if (!enabled || ring_count == 0) return;
    if (i < 0) i = 0;
    if (i >= ring_count) i = ring_count - 1;
    rewind_pop_n = i + 1;
    rewind_requested = 1;
}

/* System-menu restore: snapshot-exact, no replay needed */
static void do_menu_restore(void) {
    if (ring_count == 0) return;
    ring_count -= rewind_pop_n;
    if (ring_count < 0) ring_count = 0;
    rewind_pop_n = 1;

    int slot = (ring_base + ring_count) % REWIND_SLOTS;
    slot_copy(slot, 1);
    present_counter = slot_frame[slot];
    tick_offset     = system_ticks - slot_vticks[slot];
    presents_since_snap = 0;
    ring_count++;                        /* The restored point stays usable */

    audio_tone_voice(MIX_VOICES - 1, 1200, 80, 160);
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("[RWD] Restored snapshot (frame ");
    terminal_writedec(present_counter);
    terminal_writestring(")\n");
}

/* Newest snapshot at or before the target frame, or -1 */
static int slot_for_frame(uint32_t target) {
    for (int i = ring_count - 1; i >= 0; i--) {
        int slot = (ring_base + i) % REWIND_SLOTS;
        if (slot_frame[slot] <= target) return slot;
    }
    return -1;
}

/* Begin one scrub hop: restore + fast-replay to 'target' */
static void start_step(uint32_t target) {
    int slot = slot_for_frame(target);
    if (slot < 0) return;

    slot_copy(slot, 1);
    replay_cursor = slot_frame[slot];
    replay_until  = target;
    just_restored = 1;
    mode = MODE_REPLAY;
    audio_tone_voice(MIX_VOICES - 1,
                     (uint32_t)(500 + (scrub_anchor - target) % 400), 25, 90);
}

/* Resume live play at frame F of the (rewritten) timeline */
static void finalize_scrub(uint32_t f) {
    scrubbing = 0;
    arrived   = 0;
    mode      = MODE_LIVE;

    if (f == scrub_anchor) return;      /* Released without stepping */

    present_counter = f;
    /* Clock continuity: the frame on screen was computed from the
     * observations in recs[f-1] — pick up virtual time from there */
    tick_offset = system_ticks - recs[(f ? f - 1 : 0) % RECORDS].vticks;
    /* Snapshots newer than F describe a discarded future */
    while (ring_count > 0 &&
           slot_frame[(ring_base + ring_count - 1) % REWIND_SLOTS] > f)
        ring_count--;
    presents_since_snap = 0;

    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("[RWD] Scrubbed to frame ");
    terminal_writedec(f);
    terminal_writestring(" (was ");
    terminal_writedec(scrub_anchor);
    terminal_writestring(")\n");
}

/* ──────── Syscall-facing hooks ──────── */

int rewind_replaying(void) {
    return enabled && mode == MODE_REPLAY &&
           current_task && current_task->id == owner_id;
}

int rewind_busy(void) {
    return enabled && (scrubbing || mode != MODE_LIVE);
}

int rewind_should_blit(void) {
    return !enabled || blit_now;
}

void rewind_adopt_ticks(uint32_t vticks) {
    tick_offset = system_ticks - vticks;
}

uint32_t rewind_ticks(void) {
    /* Attract replay: feed the recorded seed tick (pre-play, so
     * arcade_init seeds the PRNG identically) then per-frame vticks. */
    if (attract_is_current())
        return attract_play ? demo_play[attract_cursor].vticks : attract_seed;

    uint32_t v;
    if (!enabled || !current_task || current_task->id != owner_id)
        v = system_ticks;
    else if (mode == MODE_REPLAY)
        v = recs[replay_cursor % RECORDS].vticks;
    else
        v = system_ticks - tick_offset;
    /* Track the last tick handed out: at a game's first present this is
     * the seed it read in arcade_init (nothing else runs meanwhile). */
    g_last_tick = v;
    return v;
}

int rewind_feed_pad(int index, pad_state_t* st) {
    if (index < 0 || index > 1) return 0;

    /* Attract replay: hand the game its recorded input; a real button
     * on the actual pad ends the demo. */
    if (attract_is_current()) {
        if (index == 0 && (st->buttons != 0)) attract_cancel = 1;
        *st = demo_play[attract_play ? attract_cursor : 0].pad[index];
        return 1;
    }

    if (!enabled || !current_task || current_task->id != owner_id) return 0;

    if (mode == MODE_REPLAY) {
        *st = recs[replay_cursor % RECORDS].pad[index];
        return 1;
    }
    recs[present_counter % RECORDS].pad[index] = *st;
    if (cap_active && present_counter < DEMO_FRAMES)
        demo_cap[present_counter].pad[index] = *st;
    return 0;
}

void rewind_on_present(void) {
    blit_now = 1;
    if (!enabled || !current_task) return;

    /* New process on the screen: rebuild the page list, drop history */
    if (current_task->id != owner_id) {
        owner_id = current_task->id;
        npages   = paging_collect_user_rw(pages, REWIND_MAX_PAGES);
        ring_base = ring_count = 0;
        presents_since_snap = 0;
        rewind_requested = 0;
        present_counter  = 0;
        tick_offset      = 0;
        mode = MODE_LIVE;
        scrubbing = arrived = just_restored = 0;
        recs[0].vticks = system_ticks;

        /* Is this the game a queued attract demo is waiting for? */
        if (attract_active && !attract_play &&
            strcmp(rw_basename(current_task->name), attract_game) == 0) {
            attract_play   = 1;             /* Switch from seed to per-frame */
            attract_cursor = 0;
            attract_owner  = current_task->id;
            cap_active     = 0;
        } else {
            /* Fresh normal play: start capturing a demo of this run */
            cap_active = 1;
            demo_cap_n = 1;
            demo_cap[0].vticks = system_ticks;
            demo_seed  = g_last_tick;       /* Seed the game just read */
        }

        take_snapshot();
        return;
    }

    /* Attract replay: advance the demo, blits ON, real-time */
    if (attract_active && attract_play && current_task->id == attract_owner) {
        attract_cursor++;
        if (attract_cursor >= demo_play_n || attract_cancel ||
            system_ticks > attract_deadline)
            attract_end();                  /* No return */
        return;
    }

    if (mode == MODE_REPLAY) {
        if (just_restored) just_restored = 0;   /* Shows the snapshot frame */
        else               replay_cursor++;
        if (replay_cursor >= replay_until) {
            arrived  = 1;       /* Blit this frame; hold logic in post_blit */
            blit_now = 1;
        } else {
            blit_now = 0;       /* Fast-forwarding: stay off the screen */
        }
        return;
    }

    /* MODE_LIVE */
    present_counter++;
    recs[present_counter % RECORDS].vticks = system_ticks - tick_offset;
    if (cap_active && present_counter < DEMO_FRAMES) {
        demo_cap[present_counter].vticks = system_ticks - tick_offset;
        demo_cap_n = present_counter + 1;
    }

    /* Chord (raw — the pad filter hides it from the game): open a
     * scrub session. The hold loop runs after this frame's blit. */
    if (!scrubbing && ring_count > 0 &&
        (gamepad_raw_buttons(0) & CHORD) == CHORD) {
        scrubbing    = 1;
        scrub_anchor = present_counter;
        audio_stop_voice(-1);
        terminal_writestring("[RWD] Scrub begin\n");
        return;
    }

    if (rewind_requested) {              /* System-menu restore */
        rewind_requested = 0;
        do_menu_restore();
        return;
    }

    if (++presents_since_snap >= SNAP_INTERVAL) {
        presents_since_snap = 0;
        take_snapshot();
    }
}

/* After the blit: the scrub hold-loop. The game is parked inside its
 * own SYS_GFX_PRESENT showing frame F; we watch the chord and either
 * hop further back (restore + replay — the game resumes executing) or
 * finalize and hand the timeline back. */
void rewind_post_blit(void) {
    if (!enabled || !scrubbing) return;
    if (mode == MODE_REPLAY && !arrived) return;     /* Still seeking */
    arrived = 0;

    uint32_t f = (mode == MODE_REPLAY) ? replay_until : present_counter;


    uint32_t next_hop = system_ticks + SCRUB_STEP_MS;
    for (;;) {
        /* Overlay: "<< REWIND" — redrawn every pass so it survives
         * page flips and screendump races */
        {
            uint32_t behind = (scrub_anchor - f) * 16;   /* frame_ms */
            char msg[24] = "<< REWIND ";
            int n = 10;
            uint32_t s10 = behind / 100;                 /* Tenths of seconds */
            if (s10 >= 100) msg[n++] = (char)('0' + (s10 / 100) % 10);
            msg[n++] = (char)('0' + (s10 / 10) % 10);
            msg[n++] = '.';
            msg[n++] = (char)('0' + s10 % 10);
            msg[n++] = 'S';
            msg[n]   = '\0';
            fb_overlay_rect(12, 12, 232, 28, 0x101430);
            fb_overlay_text(20, 18, msg, 0xFFDC50, 2);
        }

        usb_poll();
        if ((gamepad_raw_buttons(0) & CHORD) != CHORD) {
            finalize_scrub(f);
            return;
        }
        if (system_ticks >= next_hop) {
            uint32_t oldest = slot_frame[ring_base];
            if (ring_count > 0 && f > oldest) {
                uint32_t target = (f > oldest + SCRUB_STEP_FRAMES)
                                ? f - SCRUB_STEP_FRAMES : oldest;
                start_step(target);
                return;                  /* Game resumes, replaying */
            }
            next_hop = system_ticks + SCRUB_STEP_MS;   /* Pinned at oldest */
        }
        asm volatile("sti\nhlt");
    }
}

int rewind_filter_pad(int index, pad_state_t* st) {
    static uint32_t cooldown_until = 0;
    static int      select_pending = 0;
    static int      menu_chord_was_down = 0;

    if (!enabled || index != 0) return 0;

    /* SELECT+START: system menu (armed here, opened at next present) */
    int mchord = (st->buttons & MENU_CHORD) == MENU_CHORD;
    if (mchord) {
        if (!menu_chord_was_down)
            sysmenu_request();
        menu_chord_was_down = 1;
        select_pending = 0;
        st->buttons = 0;
        st->lx = st->ly = st->rx = st->ry = 0;
        cooldown_until = system_ticks + 400;
        return 1;
    }
    menu_chord_was_down = 0;

    /* SELECT+L1: scrub chord. The session itself starts from the raw
     * state in rewind_on_present — here we only hide it from the game. */
    if ((st->buttons & CHORD) == CHORD) {
        select_pending = 0;
        st->buttons = 0;
        st->lx = st->ly = st->rx = st->ry = 0;
        cooldown_until = system_ticks + 400;
        return 1;
    }

    /* After release: the kernel latches press edges for slow game
     * loops, so a chord key can replay as a lone SELECT one poll
     * later — most games treat that as QUIT. Keep swallowing. */
    if (system_ticks < cooldown_until) {
        st->buttons &= (uint16_t)~(CHORD | MENU_CHORD);
        return 0;
    }

    /* SELECT alone might be the first key of a chord still forming —
     * and most games treat a bare SELECT as QUIT, so a single leaked
     * poll is fatal. Withhold it for as long as it stays held alone
     * (a chord may take arbitrarily long under load) and deliver the
     * press on RELEASE instead: a deliberate quit just lands ~100 ms
     * later, and a slow chord never leaks. */
    if (st->buttons & PAD_BTN_SELECT) {
        select_pending = 1;
        st->buttons &= (uint16_t)~PAD_BTN_SELECT;
    } else if (select_pending &&
               (st->buttons & (PAD_BTN_START | PAD_BTN_L1))) {
        /* The chord's keys straddled two slow polls: SELECT was
         * withheld and already released, and its partner only shows
         * up now (live or latched). Reassemble the chord instead of
         * delivering a lethal lone SELECT. */
        if (st->buttons & PAD_BTN_START)
            sysmenu_request();
        /* (A late L1 can't scrub-hold — swallow it silently.) */
        select_pending = 0;
        st->buttons = 0;
        st->lx = st->ly = st->rx = st->ry = 0;
        cooldown_until = system_ticks + 400;
        return 1;
    } else if (select_pending) {
        select_pending = 0;
        /* Released without a chord: this WAS a quit tap — deliver it
         * as a one-poll press edge. */
        st->buttons |= PAD_BTN_SELECT;
    }
    return 0;
}

/* ──────── Attract mode public API (see rewind.h) ──────── */

int rewind_demo_save(const char* elfbase) {
    if (demo_cap_n < DEMO_MIN) return 0;
    demo_hdr_t* h = (demo_hdr_t*)demo_io;
    h->magic   = DEMO_MAGIC;
    h->seed    = demo_seed;
    h->nframes = (uint32_t)demo_cap_n;
    memcpy(demo_io + sizeof(demo_hdr_t), demo_cap,
           (uint32_t)demo_cap_n * sizeof(frame_rec_t));

    char name[16];
    demo_name(rw_basename(elfbase), name);
    uint32_t bytes = sizeof(demo_hdr_t) + (uint32_t)demo_cap_n * sizeof(frame_rec_t);
    int r = fat32_save(name, demo_io, bytes);
    if (r == 0) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
        terminal_writestring("[ATTRACT] saved demo ");
        terminal_writestring(name);
        terminal_writestring("\n");
    }
    return r == 0;
}

int rewind_arm_attract(const char* elfbase) {
    char name[16];
    demo_name(rw_basename(elfbase), name);

    int n = fat32_load(name, demo_io, sizeof(demo_io));
    if (n < (int)sizeof(demo_hdr_t)) return 0;
    demo_hdr_t* h = (demo_hdr_t*)demo_io;
    if (h->magic != DEMO_MAGIC || h->nframes == 0 || h->nframes > DEMO_FRAMES)
        return 0;
    if ((uint32_t)n < sizeof(demo_hdr_t) + h->nframes * sizeof(frame_rec_t))
        return 0;

    demo_play_n = (int)h->nframes;
    attract_seed = h->seed;
    memcpy(demo_play, demo_io + sizeof(demo_hdr_t),
           h->nframes * sizeof(frame_rec_t));

    int i = 0;
    const char* b = rw_basename(elfbase);
    for (; b[i] && i < 23; i++) attract_game[i] = b[i];
    attract_game[i] = '\0';

    attract_active   = 1;
    attract_play     = 0;              /* Pre-play: feed the seed tick */
    attract_cursor   = 0;
    attract_cancel   = 0;
    attract_deadline = system_ticks + (uint32_t)demo_play_n * 20 + 6000;

    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("[ATTRACT] armed ");
    terminal_writestring(attract_game);
    terminal_writestring("\n");
    return 1;
}

int rewind_attract_active(void) { return attract_active; }
