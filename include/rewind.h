#ifndef REWIND_H
#define REWIND_H

#include "types.h"
#include "console_abi.h"

/*
 * ArcadeOS – kernel-level universal rewind
 *
 * Every Ring-3 game gets save-states for free, with zero cooperation
 * from its code: the kernel snapshots the process's writable pages at
 * SYS_GFX_PRESENT boundaries (between frames, the game's entire state
 * IS its memory — W^X guarantees text never changes, and restoring at
 * the same syscall boundary means no CPU context needs saving). A ring
 * of snapshots is kept ~2 s apart; holding SELECT+L1 (Tab+Q on the
 * keyboard) pops one snapshot back per press.
 *
 * The chord is a system combo: while held, SYS_PAD_READ reports a
 * cleared pad so the game can't react to SELECT (many games quit on
 * it). Disk saves made after a snapshot are NOT undone by rewinding —
 * the memory card is real, time travel isn't.
 */

/* Allocate the snapshot ring (call once at boot, after the PMM). */
void rewind_init(void);

/* Hook for SYS_GFX_PRESENT, before the blit: takes periodic snapshots,
 * and performs a requested rewind (so the very frame being presented
 * is already the restored one). */
void rewind_on_present(void);

/* Hook for SYS_PAD_READ: detects the chord on pad 0 and suppresses it
 * from the game. Returns 1 when the chord is active (pad was cleared). */
int rewind_filter_pad(int index, pad_state_t* st);

/* Snapshot introspection for the system menu. Index 0 = newest. */
int      rewind_snapshot_count(void);
uint32_t rewind_snapshot_age_ms(int i);

/* Screen thumbnail captured with each snapshot, so the menu can show
 * WHICH moment you're jumping to (THUMB_W x THUMB_H 0x00RRGGBB). */
#define REWIND_THUMB_W 104
#define REWIND_THUMB_H 78
const uint32_t* rewind_snapshot_thumb(int i);

/* Ask for snapshot i (0 = newest) to be restored at the next present —
 * everything newer than it is discarded. */
void rewind_request_restore(int i);

/* ──────── v2: frame-exact scrubbing hooks (called from syscall.c) ──── */

/* True while the game is re-executing recorded frames. Syscalls with
 * side effects (sound, saves, score, msleep) must no-op then. */
int rewind_replaying(void);

/* True during any scrub/replay activity (guards the system menu). */
int rewind_busy(void);

/* Whether this present should reach the screen (replays render dark). */
int rewind_should_blit(void);

/* Runs the scrub hold-loop after the blit (may block; may not return
 * control to the same timeline). */
void rewind_post_blit(void);

/* SYS_TICKS value for the caller: logged during replay, virtual-clock
 * shifted after a rewind so games never see time jump. */
uint32_t rewind_ticks(void);

/* SYS_PAD_READ integration: during replay, overwrites *st from the
 * frame log and returns 1; live, records *st and returns 0. */
int rewind_feed_pad(int index, pad_state_t* st);

#endif /* REWIND_H */
