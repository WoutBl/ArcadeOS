/*
 * ArcadeOS – Interrupt-Driven Keyboard Driver (IRQ1)
 */

#include "keyboard.h"
#include "idt.h"
#include "vga.h"
#include "scheduler.h"
#include "task.h"

/* ──────── Modifier state (global) ──────── */
int shift_pressed = 0;
int ctrl_pressed  = 0;
int alt_pressed   = 0;

/* ──────── Scancode lookup tables ──────── */
static const char scancode_to_ascii[] = {
    0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' '
};

static const char scancode_to_ascii_shift[] = {
    0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' '
};

/* ──────── Raw scancode hook (gamepad subsystem) ──────── */

/*
 * The gamepad driver registers a hook here to observe every raw
 * scancode (make AND break codes, including 0xE0 prefixes) before
 * the tty processing below consumes them.
 */
static void (*kb_raw_hook)(uint8_t scancode) = 0;

void keyboard_set_raw_hook(void (*hook)(uint8_t scancode)) {
    kb_raw_hook = hook;
}

/* ──────── Ring buffer for key events ──────── */
#define KEY_BUFFER_SIZE 256

static key_event_t key_buffer[KEY_BUFFER_SIZE];
static volatile uint32_t kb_head = 0;  /* ISR writes here */
static volatile uint32_t kb_tail = 0;  /* Consumer reads here */

static void kb_push(key_event_t evt) {
    uint32_t next = (kb_head + 1) % KEY_BUFFER_SIZE;
    if (next != kb_tail) {           /* Drop key if buffer full */
        key_buffer[kb_head] = evt;
        kb_head = next;
    }
}

/* ──────── Scancode processing ────────
 *
 * Shared by the PS/2 IRQ1 path and USB HID keyboards: the USB stack
 * translates boot-protocol reports into set-1 scancodes and injects
 * them here, so both keyboard types drive the same gamepad mapping
 * and the same ASCII event buffer.
 */
void keyboard_inject_scancode(uint8_t scancode) {
    /* Feed the gamepad subsystem first (it wants raw make/break codes) */
    if (kb_raw_hook)
        kb_raw_hook(scancode);

    /* Key release (bit 7 set) */
    if (scancode & 0x80) {
        uint8_t release = scancode & 0x7F;
        if (release == SCANCODE_LSHIFT || release == SCANCODE_RSHIFT)
            shift_pressed = 0;
        if (release == SCANCODE_LCTRL)
            ctrl_pressed = 0;
        if (release == SCANCODE_LALT)
            alt_pressed = 0;
        return;
    }

    /* Modifier key press */
    if (scancode == SCANCODE_LSHIFT || scancode == SCANCODE_RSHIFT) {
        shift_pressed = 1;
        return;
    }
    if (scancode == SCANCODE_LCTRL) {
        ctrl_pressed = 1;
        return;
    }
    if (scancode == SCANCODE_LALT) {
        alt_pressed = 1;
        alt_pressed = 1;
        return;
    }

    /* Intercept Ctrl+C (0x2E is the make code for 'C' on QWERTY) */
    if (ctrl_pressed && scancode == 0x2E) {
        task_kill_foreground(SIGINT);
        return;
    }

    /* Build key event */
    key_event_t evt;
    evt.scancode = scancode;
    evt.character = 0;

    if (scancode < sizeof(scancode_to_ascii)) {
        if (shift_pressed)
            evt.character = scancode_to_ascii_shift[scancode];
        else
            evt.character = scancode_to_ascii[scancode];
    }

    kb_push(evt);
}

/* ──────── IRQ1 handler (called from ISR dispatcher) ──────── */
static void keyboard_irq_handler(registers_t* regs) {
    (void)regs;  /* unused */
    keyboard_inject_scancode(inb(KEYBOARD_DATA_PORT));
}

/* ──────── Public API ──────── */
void keyboard_init(void) {
    register_interrupt_handler(IRQ1, keyboard_irq_handler);
}

key_event_t keyboard_get_key(void) {
    key_event_t evt = {0, 0};

    if (kb_tail != kb_head) {
        evt = key_buffer[kb_tail];
        kb_tail = (kb_tail + 1) % KEY_BUFFER_SIZE;
    }
    return evt;
}

char keyboard_read_blocking(void) {
    while (1) {
        key_event_t evt = keyboard_get_key();
        if (evt.scancode != 0) {
            char c = evt.character;

            /* Handle Enter */
            if (c == '\n') {
                // terminal_putchar('\n'); // Let devfs.c handle echoing
                return '\n';
            }
            /* Handle Backspace */
            if (c == '\b') {
                /* We don't echo backspace here. The TTY driver or shell will handle erasing it visually. */
                // terminal_putchar('\b'); // Let devfs.c handle echoing
                // terminal_putchar(' ');
                // terminal_putchar('\b');
                return '\b';
            }
            if (c != 0) {
                // Echo the character to screen instantly for local feedback
                // char buf[2] = {c, '\0'};
                // terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
                // terminal_writestring(buf);
                // terminal_putchar(c); // Let devfs.c handle echoing
                return c;
            }
        }
        
        /* Yield to other threads if polling fails */
        schedule();
    }
}
