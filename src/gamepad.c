/*
 * ArcadeOS – Gamepad Input Subsystem
 *
 * See gamepad.h for the source/mapping overview.
 */

#include "gamepad.h"
#include "keyboard.h"
#include "vga.h"
#include "usb.h"
#include "clock.h"

/*
 * Per-SOURCE raw state (see gamepad.h for the source/assignment split).
 * Indexed by gamepad_source_t: 0/1 = the keyboard's two virtual pads,
 * 2/3 = up to two USB HID gamepads.
 */
static pad_state_t raw[GAMEPAD_NUM_SOURCES];

/*
 * Press-edge latch per source: buttons pressed since the last
 * gamepad_get_state() call. OR-ing this into the returned state
 * guarantees that even a button tapped between two polls is observed
 * exactly once, no matter how slow the game's frame loop is.
 */
static uint16_t src_latch[GAMEPAD_NUM_SOURCES];

/* Which player slot (0/1, -1 = none) each source currently feeds. */
static int assignment[GAMEPAD_NUM_SOURCES];

/* USB device addresses currently claiming GAMEPAD_SRC_USB_0/1. */
static uint8_t usb_addr[2];
static int     usb_addr_valid[2];

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
    pad_state_t* kb_b = &raw[GAMEPAD_SRC_KEYBOARD_B];
    int16_t lx = 0, ly = 0;
    uint16_t dpad = 0;
    if (wasd_held & 0x2) { lx = -32767; dpad |= PAD_BTN_LEFT;  }   /* A */
    if (wasd_held & 0x8) { lx =  32767; dpad |= PAD_BTN_RIGHT; }   /* D */
    if (wasd_held & 0x1) { ly = -32767; dpad |= PAD_BTN_UP;    }   /* W */
    if (wasd_held & 0x4) { ly =  32767; dpad |= PAD_BTN_DOWN;  }   /* S */
    kb_b->lx = lx;
    kb_b->ly = ly;
    uint16_t newly = (uint16_t)(dpad & ~(kb_b->buttons));
    kb_b->buttons = (uint16_t)((kb_b->buttons &
        ~(PAD_BTN_UP | PAD_BTN_DOWN | PAD_BTN_LEFT | PAD_BTN_RIGHT)) | dpad);
    src_latch[GAMEPAD_SRC_KEYBOARD_B] |= newly;
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
                raw[GAMEPAD_SRC_KEYBOARD_B].buttons &= (uint16_t)~bit1;
            } else {
                raw[GAMEPAD_SRC_KEYBOARD_B].buttons |= bit1;
                src_latch[GAMEPAD_SRC_KEYBOARD_B]    |= bit1;
            }
            return;
        }

        bit = map_plain_scancode(code);
        if (bit == 0) return;
    }

    if (released) {
        raw[GAMEPAD_SRC_KEYBOARD_A].buttons &= (uint16_t)~bit;
    } else {
        raw[GAMEPAD_SRC_KEYBOARD_A].buttons |= bit;
        src_latch[GAMEPAD_SRC_KEYBOARD_A]    |= bit;
    }
}

/* ──────── Public API ──────── */

void gamepad_init(void) {
    memset(raw, 0, sizeof(raw));
    memset(src_latch, 0, sizeof(src_latch));
    memset(usb_addr_valid, 0, sizeof(usb_addr_valid));

    /* The keyboard provides TWO virtual pads for local 2-player,
     * assigned to P1/P2 by default until a USB pad or an explicit
     * gamepad_assign() call changes that. */
    raw[GAMEPAD_SRC_KEYBOARD_A].connected = 1;
    raw[GAMEPAD_SRC_KEYBOARD_A].source    = PAD_SOURCE_KEYBOARD;
    raw[GAMEPAD_SRC_KEYBOARD_B].connected = 1;
    raw[GAMEPAD_SRC_KEYBOARD_B].source    = PAD_SOURCE_KEYBOARD;
    assignment[GAMEPAD_SRC_KEYBOARD_A] = 0;
    assignment[GAMEPAD_SRC_KEYBOARD_B] = 1;
    assignment[GAMEPAD_SRC_USB_0]      = -1;
    assignment[GAMEPAD_SRC_USB_1]      = -1;

    keyboard_set_raw_hook(gamepad_kb_hook);

    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("[PAD] Controller stack ready (keyboard: P1+P2)\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
    terminal_writestring("[PAD] P1: arrows X/Z/C/V Enter/Tab  P2: WASD R/T/F/G\n");
}

void gamepad_get_state(int slot, pad_state_t* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (slot < 0 || slot > 1) return;

    for (int s = 0; s < GAMEPAD_NUM_SOURCES; s++) {
        if (assignment[s] != slot || !raw[s].connected) continue;

        out->connected = 1;
        out->source    = raw[s].source;
        out->buttons  |= raw[s].buttons;

        /* Analog axes: the physical stick wins outside its dead zone */
        if (raw[s].lx > 4096 || raw[s].lx < -4096) out->lx = raw[s].lx;
        if (raw[s].ly > 4096 || raw[s].ly < -4096) out->ly = raw[s].ly;
        if (raw[s].rx) out->rx = raw[s].rx;
        if (raw[s].ry) out->ry = raw[s].ry;
        if (raw[s].lt) out->lt = raw[s].lt;
        if (raw[s].rt) out->rt = raw[s].rt;

        /* Deliver latched presses once, then clear them */
        out->buttons |= src_latch[s];
        src_latch[s] = 0;
    }
}

void gamepad_relatch(int slot, uint16_t buttons) {
    if (slot < 0 || slot > 1) return;
    for (int s = 0; s < GAMEPAD_NUM_SOURCES; s++)
        if (assignment[s] == slot) src_latch[s] |= buttons;
}

uint16_t gamepad_raw_buttons(int slot) {
    if (slot < 0 || slot > 1) return 0;
    uint16_t b = 0;
    for (int s = 0; s < GAMEPAD_NUM_SOURCES; s++)
        if (assignment[s] == slot && raw[s].connected) b |= raw[s].buttons;
    return b;
}

void gamepad_feed_usb(int source, uint16_t buttons,
                      int16_t lx, int16_t ly, int16_t rx, int16_t ry,
                      uint8_t lt, uint8_t rt) {
    if (source < GAMEPAD_SRC_USB_0 || source >= GAMEPAD_NUM_SOURCES) return;

    src_latch[source]      |= (uint16_t)(buttons & ~raw[source].buttons);
    raw[source].connected   = 1;
    raw[source].source      = PAD_SOURCE_USB;
    raw[source].buttons     = buttons;
    raw[source].lx = lx;  raw[source].ly = ly;
    raw[source].rx = rx;  raw[source].ry = ry;
    raw[source].lt = lt;  raw[source].rt = rt;
}

/* ──────── Controller assignment ──────── */

void gamepad_assign(int source, int player_slot) {
    if (source < 0 || source >= GAMEPAD_NUM_SOURCES) return;
    if (player_slot < -1 || player_slot > 1) return;
    assignment[source] = player_slot;
}

int gamepad_assignment(int source) {
    if (source < 0 || source >= GAMEPAD_NUM_SOURCES) return -1;
    return assignment[source];
}

int gamepad_source_connected(int source) {
    if (source < 0 || source >= GAMEPAD_NUM_SOURCES) return 0;
    return raw[source].connected;
}

uint16_t gamepad_unassigned_latched(void) {
    uint16_t b = 0;
    for (int s = 0; s < GAMEPAD_NUM_SOURCES; s++) {
        if (assignment[s] >= 0) continue;   /* Already covered by its slot's own edge detection */
        b |= src_latch[s];
        src_latch[s] = 0;
    }
    return b;
}

void gamepad_source_label(int source, char* out, int cap) {
    static const char* const labels[GAMEPAD_NUM_SOURCES] = {
        "KEYBOARD A", "KEYBOARD B", "USB PAD 1", "USB PAD 2",
    };
    if (!out || cap <= 0) return;
    const char* s = (source >= 0 && source < GAMEPAD_NUM_SOURCES) ? labels[source] : "?";
    int i = 0;
    while (s[i] && i < cap - 1) { out[i] = s[i]; i++; }
    out[i] = '\0';
}

int gamepad_usb_source_for_addr(uint8_t addr) {
    for (int i = 0; i < 2; i++)
        if (usb_addr_valid[i] && usb_addr[i] == addr) return GAMEPAD_SRC_USB_0 + i;

    for (int i = 0; i < 2; i++) {
        if (usb_addr_valid[i]) continue;
        usb_addr_valid[i] = 1;
        usb_addr[i] = addr;
        int source = GAMEPAD_SRC_USB_0 + i;
        raw[source].connected = 0;   /* becomes 1 once a report actually arrives */

        /* Claim the first player slot not already held by another
         * CONNECTED USB pad — never bump a second real controller —
         * demoting whichever keyboard source defaults to that slot.
         * A plugged-in pad "just works" as long as one is available;
         * with both slots taken by real pads it's left unassigned
         * (spectator) until someone explicitly assigns it. */
        int usb_holds[2] = {0, 0};
        for (int t = GAMEPAD_SRC_USB_0; t < GAMEPAD_NUM_SOURCES; t++)
            if (t != source && assignment[t] >= 0 && raw[t].connected) usb_holds[assignment[t]] = 1;

        int slot = !usb_holds[0] ? 0 : !usb_holds[1] ? 1 : -1;
        if (slot >= 0) {
            int kb = (slot == 0) ? GAMEPAD_SRC_KEYBOARD_A : GAMEPAD_SRC_KEYBOARD_B;
            if (assignment[kb] == slot) assignment[kb] = -1;
        }
        assignment[source] = slot;
        return source;
    }
    return -1;   /* both USB source slots already claimed */
}

void gamepad_usb_disconnect_addr(uint8_t addr) {
    for (int i = 0; i < 2; i++) {
        if (!usb_addr_valid[i] || usb_addr[i] != addr) continue;
        int source = GAMEPAD_SRC_USB_0 + i;
        int slot = assignment[source];

        raw[source].connected = 0;
        raw[source].buttons   = 0;
        assignment[source]    = -1;
        src_latch[source]     = 0;
        usb_addr_valid[i]     = 0;

        /* Hand the slot back to its default keyboard source, unless
         * something else has since claimed it explicitly. */
        if (slot >= 0) {
            int kb = (slot == 0) ? GAMEPAD_SRC_KEYBOARD_A : GAMEPAD_SRC_KEYBOARD_B;
            if (assignment[kb] < 0) assignment[kb] = slot;
        }
    }
}

/* ──────── Rumble ────────
 *
 * A short haptic buzz for game events (hit, score, game over) — see
 * sdk/arcade.h's arcade_rumble(). Auto-stops after the requested
 * duration via gamepad_rumble_idle_check() (called from the idle
 * task), so a game can fire-and-forget without tracking a timer or
 * risking a stuck motor if it exits mid-buzz. */

static int      rumble_active    = 0;
static uint32_t rumble_stop_tick = 0;

void gamepad_set_rumble(int player_slot, uint8_t strength, uint32_t ms) {
    if (player_slot < 0 || player_slot > 1) return;

    /* Only a USB source can actually buzz; a keyboard-only slot is a
     * silent no-op rather than an error. Logged once (not every call —
     * this is a hot path) so a "no buzz" report is diagnosable: did
     * gamepad.c even think a USB pad was on this slot, or did it get
     * that far and usb_set_rumble() itself have nothing to send to? */
    int has_usb = 0;
    for (int s = GAMEPAD_SRC_USB_0; s < GAMEPAD_NUM_SOURCES; s++)
        if (assignment[s] == player_slot && raw[s].connected) has_usb = 1;
    if (!has_usb) {
        static int warned = 0;
        if (!warned) {
            warned = 1;
            terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK));
            terminal_writestring("[PAD] Rumble requested but that player slot has no USB pad\n");
        }
        return;
    }

    if (ms > 2000) ms = 2000;   /* Safety cap, same spirit as sfx_tone_v's */
    usb_set_rumble(strength, strength);
    rumble_active    = 1;
    rumble_stop_tick = system_ticks + ms;
}

void gamepad_rumble_idle_check(void) {
    if (rumble_active && system_ticks >= rumble_stop_tick) {
        usb_set_rumble(0, 0);
        rumble_active = 0;
    }
}
