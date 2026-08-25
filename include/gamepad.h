#ifndef GAMEPAD_H
#define GAMEPAD_H

#include "types.h"
#include "console_abi.h"

/*
 * ArcadeOS – Gamepad Input Subsystem
 *
 * Two independent layers:
 *
 *   1. SOURCES — physical inputs, always tracked regardless of who
 *      they're playing for: the keyboard's two virtual pads (A =
 *      arrows/X/Z/C/V/Enter/Tab/Q/E, B = WASD+R/T/F/G) plus up to two
 *      USB HID gamepads (the USB stack only has two device slots, see
 *      usb.h). See gamepad_source_t.
 *
 *   2. ASSIGNMENT — which source (if any) feeds player slot 0 (P1) or
 *      1 (P2), the only two slots any game actually reads. Reassignable
 *      at any time — e.g. from a launcher menu — so a controller
 *      plugged in mid-session can be handed to either player, or two
 *      people can swap seats without touching a game's code.
 *
 * Default behavior needs no setup: keyboard A/B start as P1/P2; the
 * first USB pad to send a report claims P1 (demoting keyboard A), the
 * second claims P2 (demoting keyboard B); unplugging a USB pad hands
 * its slot back to the keyboard source that originally held it, if
 * that source is still unassigned. gamepad_assign() overrides any of
 * this explicitly.
 *
 * Games read pad state through the SYS_PAD_READ syscall, which calls
 * gamepad_get_state() with a player slot (0 or 1) — never a source id.
 * State updates are interrupt-driven (keyboard) or polled from the USB
 * controllers, so reads never block.
 */

#define GAMEPAD_NUM_SOURCES 4

typedef enum {
    GAMEPAD_SRC_KEYBOARD_A = 0,   /* Arrows + X/Z/C/V + Enter/Tab + Q/E */
    GAMEPAD_SRC_KEYBOARD_B = 1,   /* WASD + R/T/F/G */
    GAMEPAD_SRC_USB_0      = 2,
    GAMEPAD_SRC_USB_1      = 3,
} gamepad_source_t;

void gamepad_init(void);

/* Snapshot the state of player slot 'slot' (0 = P1, 1 = P2) into 'out'
 * — the OR of whichever source(s) are currently assigned to it
 * (zeroed if invalid or nothing assigned). */
void gamepad_get_state(int slot, pad_state_t* out);

/* Re-arm press-edge latches for buttons a kernel filter withheld from
 * the poll that consumed them (e.g. the rewind chord's SELECT grace),
 * so the game still observes the press on a later poll. 'slot' is a
 * player slot, same as gamepad_get_state. */
void gamepad_relatch(int slot, uint16_t buttons);

/* Current raw held-button mask for a player slot WITHOUT consuming
 * press-edge latches — for kernel code (rewind scrub) that must watch
 * a chord while the game's own input stream is being replayed or
 * withheld. */
uint16_t gamepad_raw_buttons(int slot);

/* Called by the USB HID layer when a report arrives for the given
 * source (GAMEPAD_SRC_USB_0/1, from gamepad_usb_source_for_addr). */
void gamepad_feed_usb(int source, uint16_t buttons,
                      int16_t lx, int16_t ly, int16_t rx, int16_t ry,
                      uint8_t lt, uint8_t rt);

/* ──────── Controller assignment (see above) ──────── */

/* Assign source 'source' to player slot 'player_slot' (0 or 1), or -1
 * to unassign it (spectator / not playing). */
void gamepad_assign(int source, int player_slot);

/* Which player slot 'source' currently feeds: -1, 0, or 1. */
int  gamepad_assignment(int source);

int  gamepad_source_connected(int source);

/* OR of every source's press-edge latch, source-assignment be damned,
 * clearing them as it goes — a catch-all so the controller-assignment
 * screen is always escapable. Sources feeding a player slot are
 * already drained into that slot's gamepad_get_state() by the time
 * this runs (once per frame, same as any other pad read), so in
 * practice this only ever reports presses from a FULLY UNASSIGNED
 * source, which gamepad_get_state() never delivers to anyone —
 * without it, unassigning your only working controller would strand
 * it with no way to reassign itself back. */
uint16_t gamepad_any_latched(void);

/* Copies a short display label ("KEYBOARD A", "USB PAD 1", ...) for
 * 'source' into 'out' (a NUL-terminated string of at most cap-1 chars). */
void gamepad_source_label(int source, char* out, int cap);

/* USB stack hook: returns the stable source id for a USB device
 * address, registering it (and auto-assigning the first free player
 * slot per the default behavior above) the first time it's seen.
 * Returns -1 if both USB source slots are already taken by other
 * addresses. */
int  gamepad_usb_source_for_addr(uint8_t addr);

/* USB stack hook: called when a USB device disconnects, so its source
 * frees up (and its player slot reverts to keyboard if unclaimed). */
void gamepad_usb_disconnect_addr(uint8_t addr);

#endif /* GAMEPAD_H */
