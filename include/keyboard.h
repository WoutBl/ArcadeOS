#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "types.h"

/* Keyboard I/O ports */
#define KEYBOARD_DATA_PORT   0x60
#define KEYBOARD_STATUS_PORT 0x64

/* Special scancodes */
#define SCANCODE_ESC     0x01
#define SCANCODE_LCTRL   0x1D
#define SCANCODE_LSHIFT  0x2A
#define SCANCODE_RSHIFT  0x36
#define SCANCODE_LALT    0x38
#define SCANCODE_CAPS    0x3A
#define SCANCODE_F1      0x3B
#define SCANCODE_F2      0x3C
#define SCANCODE_F3      0x3D
#define SCANCODE_F4      0x3E
#define SCANCODE_F5      0x3F
#define SCANCODE_F6      0x40
#define SCANCODE_F7      0x41
#define SCANCODE_F8      0x42
#define SCANCODE_F9      0x43
#define SCANCODE_F10     0x44
#define SCANCODE_UP      0x48
#define SCANCODE_PGUP    0x49
#define SCANCODE_LEFT    0x4B
#define SCANCODE_RIGHT   0x4D
#define SCANCODE_DOWN    0x50
#define SCANCODE_PGDOWN  0x51
#define SCANCODE_F11     0x57
#define SCANCODE_F12     0x58

/* Key event returned to consumers */
typedef struct {
    uint8_t scancode;
    char    character;
} key_event_t;

/* Modifier state */
extern int shift_pressed;
extern int ctrl_pressed;
extern int alt_pressed;

/* Initialize keyboard (registers IRQ1 handler) */
void keyboard_init(void);

/*
 * Register a hook that receives every raw scancode from IRQ1
 * (including break codes and 0xE0 prefixes). Used by gamepad.c.
 */
void keyboard_set_raw_hook(void (*hook)(uint8_t scancode));

/* Process one set-1 scancode as if it came from the PS/2 port. Used by
 * the USB stack to feed HID boot-keyboard input through the same
 * gamepad mapping and ASCII pipeline. */
void keyboard_inject_scancode(uint8_t scancode);

/*
 * Non-blocking: returns the next key event from the ring buffer.
 * If the buffer is empty, returns {0, 0}.
 */
key_event_t keyboard_get_key(void);

/*
 * Blocking: waits for keyboard input and returns the ASCII character.
 * Echos non-special characters automatically to the terminal. 
 */
char keyboard_read_blocking(void);

#endif /* KEYBOARD_H */
