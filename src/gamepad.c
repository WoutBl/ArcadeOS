/*
 * ArcadeOS – Gamepad Input Subsystem
 *
 * See gamepad.h for the source/mapping overview.
 */

#include "gamepad.h"
#include "keyboard.h"
#include "vga.h"

/*
 * Logical pad states, updated by the input sources.
 * pads[]     – keyboard-mapped state: the keyboard is SPLIT into two
 *              virtual pads for local 2-player. Pad 0 = arrows + X/Z/C/V
 *              + Enter/Tab + Q/E; pad 1 = WASD (D-pad + stick) + R/T/F/G.
 * usb_pads[] – USB HID gamepad state (DualShock 4 etc.), merged into
 *              pad 0, so a physical controller is always player 1.
 */
static pad_state_t pads[PAD_MAX_CONTROLLERS];
static pad_state_t usb_pads[PAD_MAX_CONTROLLERS];

/*
 * Press-edge latch: buttons pressed since the last gamepad_get_state()
 * call. OR-ing this into the returned state guarantees that even a
 * button tapped between two polls is observed exactly once, no matter
 * how slow the game's frame loop is.
 */
static uint16_t pad_latch[PAD_MAX_CONTROLLERS];

/* ──────── Keyboard-mapped virtual pad (pad 0) ──────── */

/*
 * The raw scancode hook fires on every make/break code from IRQ1,
 * including 0xE0-prefixed extended codes (arrow keys).
 */
static int kb_extended = 0;   /* Saw an 0xE0 prefix */

/* Scancode → button-bit mapping for the non-extended set */
static uint16_t map_plain_scancode(uint8_t sc) {
    switch (sc) {
        case 0x2D: return PAD_BTN_A;       /* X key */
        case 0x2C: return PAD_BTN_B;       /* Z key */
        case 0x2E: return PAD_BTN_X;       /* C key */
        case 0x2F: return PAD_BTN_Y;       /* V key */
        case 0x1C: return PAD_BTN_START;   /* Enter */
        case 0x0F: return PAD_BTN_SELECT;  /* Tab */
        case 0x10: return PAD_BTN_L1;      /* Q key */
        case 0x12: return PAD_BTN_R1;      /* E key */
        /* Keypad arrows double as D-pad when NumLock is off */
        case SCANCODE_UP:    return PAD_BTN_UP;
        case SCANCODE_DOWN:  return PAD_BTN_DOWN;
        case SCANCODE_LEFT:  return PAD_BTN_LEFT;
        case SCANCODE_RIGHT: return PAD_BTN_RIGHT;
    }
    return 0;
}

/* Pad 1 button map: R/T/F/G around WASD */
static uint16_t map_pad1_scancode(uint8_t sc) {
    switch (sc) {
        case 0x13: return PAD_BTN_A;       /* R key */
        case 0x14: return PAD_BTN_B;       /* T key */
        case 0x21: return PAD_BTN_X;       /* F key */
        case 0x22: return PAD_BTN_Y;       /* G key */
    }
    return 0;
}

/* WASD = pad 1 D-pad, and drives its left stick digitally too so
 * stick-reading games work for player 2 */
static uint16_t wasd_held = 0;   /* Bits: 1=W 2=A 4=S 8=D */

static void update_pad1_dirs(void) {
    int16_t lx = 0, ly = 0;
    uint16_t dpad = 0;
    if (wasd_held & 0x2) { lx = -32767; dpad |= PAD_BTN_LEFT;  }   /* A */
    if (wasd_held & 0x8) { lx =  32767; dpad |= PAD_BTN_RIGHT; }   /* D */
    if (wasd_held & 0x1) { ly = -32767; dpad |= PAD_BTN_UP;    }   /* W */
    if (wasd_held & 0x4) { ly =  32767; dpad |= PAD_BTN_DOWN;  }   /* S */
    pads[1].lx = lx;
    pads[1].ly = ly;
    uint16_t newly = (uint16_t)(dpad & ~(pads[1].buttons));
    pads[1].buttons = (uint16_t)((pads[1].buttons &
        ~(PAD_BTN_UP | PAD_BTN_DOWN | PAD_BTN_LEFT | PAD_BTN_RIGHT)) | dpad);
    pad_latch[1] |= newly;
}

static void gamepad_kb_hook(uint8_t scancode) {
    if (scancode == 0xE0) {       /* Extended prefix – next code is extended */
        kb_extended = 1;
        return;
    }

    int      released = scancode & 0x80;
    uint8_t  code     = scancode & 0x7F;
    int      extended = kb_extended;
    kb_extended = 0;

    /* Extended set: the "grey" arrow keys (same low codes as keypad) */
    uint16_t bit = 0;
    if (extended) {
        switch (code) {
            case SCANCODE_UP:    bit = PAD_BTN_UP;    break;
            case SCANCODE_DOWN:  bit = PAD_BTN_DOWN;  break;
            case SCANCODE_LEFT:  bit = PAD_BTN_LEFT;  break;
            case SCANCODE_RIGHT: bit = PAD_BTN_RIGHT; break;
            default: return;
        }
    } else {
        /* WASD → pad 1 D-pad + stick */
        uint16_t wasd_bit = 0;
        switch (code) {
            case 0x11: wasd_bit = 0x1; break;   /* W */
            case 0x1E: wasd_bit = 0x2; break;   /* A */
            case 0x1F: wasd_bit = 0x4; break;   /* S */
            case 0x20: wasd_bit = 0x8; break;   /* D */
        }
        if (wasd_bit) {
            if (released) wasd_held &= (uint16_t)~wasd_bit;
            else          wasd_held |= wasd_bit;
            update_pad1_dirs();
            return;
        }

        /* R/T/F/G → pad 1 face buttons */
        uint16_t bit1 = map_pad1_scancode(code);
        if (bit1) {
            if (released) {
                pads[1].buttons &= (uint16_t)~bit1;
            } else {
                pads[1].buttons |= bit1;
                pad_latch[1]    |= bit1;
            }
            return;
        }

        bit = map_plain_scancode(code);
        if (bit == 0) return;
    }

    if (released) {
        pads[0].buttons &= (uint16_t)~bit;
    } else {
        pads[0].buttons |= bit;
        pad_latch[0]    |= bit;
    }
}

/* ──────── Public API ──────── */

void gamepad_init(void) {
    memset(pads, 0, sizeof(pads));
    memset(usb_pads, 0, sizeof(usb_pads));

    /* The keyboard provides TWO virtual pads for local 2-player */
    pads[0].connected = 1;
    pads[0].source    = PAD_SOURCE_KEYBOARD;
    pads[1].connected = 1;
    pads[1].source    = PAD_SOURCE_KEYBOARD;

    keyboard_set_raw_hook(gamepad_kb_hook);

    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("[PAD] Controller stack ready (keyboard: pads 0+1)\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
    terminal_writestring("[PAD] P1: arrows X/Z/C/V Enter/Tab  P2: WASD R/T/F/G\n");
}

void gamepad_get_state(int index, pad_state_t* out) {
    if (!out) return;
    if (index < 0 || index >= PAD_MAX_CONTROLLERS) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = pads[index];

    /* Merge in the USB controller mapped to this pad index */
    if (usb_pads[index].connected) {
        out->connected = 1;
        out->source    = PAD_SOURCE_USB;
        out->buttons  |= usb_pads[index].buttons;

        /* Analog axes: the physical stick wins outside its dead zone */
        if (usb_pads[index].lx > 4096 || usb_pads[index].lx < -4096) out->lx = usb_pads[index].lx;
        if (usb_pads[index].ly > 4096 || usb_pads[index].ly < -4096) out->ly = usb_pads[index].ly;
        out->rx = usb_pads[index].rx;
        out->ry = usb_pads[index].ry;
        out->lt = usb_pads[index].lt;
        out->rt = usb_pads[index].rt;
    }

    /* Deliver latched presses once, then clear them */
    out->buttons |= pad_latch[index];
    pad_latch[index] = 0;
}

void gamepad_relatch(int index, uint16_t buttons) {
    if (index < 0 || index >= PAD_MAX_CONTROLLERS) return;
    pad_latch[index] |= buttons;
}

void gamepad_feed_usb(int index, uint16_t buttons,
                      int16_t lx, int16_t ly, int16_t rx, int16_t ry,
                      uint8_t lt, uint8_t rt) {
    if (index < 0 || index >= PAD_MAX_CONTROLLERS) return;

    usb_pads[index].connected = 1;
    usb_pads[index].source    = PAD_SOURCE_USB;
    pad_latch[index]         |= (uint16_t)(buttons & ~usb_pads[index].buttons);
    usb_pads[index].buttons   = buttons;
    usb_pads[index].lx = lx;  usb_pads[index].ly = ly;
    usb_pads[index].rx = rx;  usb_pads[index].ry = ry;
    usb_pads[index].lt = lt;  usb_pads[index].rt = rt;
}
