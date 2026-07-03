#ifndef GAMEPAD_H
#define GAMEPAD_H

#include "types.h"
#include "console_abi.h"

/*
 * ArcadeOS – Gamepad Input Subsystem
 *
 * Merges all controller sources into PAD_MAX_CONTROLLERS logical pads:
 *
 *   Pad 0: keyboard-mapped virtual pad (always connected – QEMU dev loop)
 *          Arrows = D-pad     WASD  = left analog stick
 *          X = A   Z = B      C = X     V = Y
 *          Enter = START      Tab = SELECT
 *          Q = L1  E = R1
 *   Pad 1+: USB HID gamepads (fed by the USB stack via gamepad_feed_usb)
 *
 * Games read pad state through the SYS_PAD_READ syscall, which calls
 * gamepad_get_state(). State updates are interrupt-driven (keyboard) or
 * polled from the USB controllers, so reads never block.
 */

void gamepad_init(void);

/* Snapshot the state of pad 'index' into 'out' (zeroed if invalid) */
void gamepad_get_state(int index, pad_state_t* out);

/* Called by the USB HID layer when a report for pad 'index' arrives */
void gamepad_feed_usb(int index, uint16_t buttons,
                      int16_t lx, int16_t ly, int16_t rx, int16_t ry,
                      uint8_t lt, uint8_t rt);

#endif /* GAMEPAD_H */
