/*
 * ArcadeOS – USB Host Stack Core
 *
 * Detects host controllers via PCI, dispatches to the per-type drivers,
 * and decodes HID input reports from enumerated devices. See usb.h for
 * the architecture overview.
 */

#include "usb.h"
#include "vga.h"
#include "gamepad.h"
#include "keyboard.h"
#include "console_abi.h"

static usb_controller_t controllers[USB_MAX_CONTROLLERS];
static int              num_controllers = 0;

static const char* hc_type_name(usb_hc_type_t t) {
    switch (t) {
        case USB_HC_UHCI: return "UHCI (USB 1.1)";
        case USB_HC_OHCI: return "OHCI (USB 1.1)";
        case USB_HC_EHCI: return "EHCI (USB 2.0)";
        case USB_HC_XHCI: return "xHCI (USB 3.x)";
    }
    return "unknown";
}

void usb_init(void) {
    num_controllers = 0;

    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
    terminal_writestring("[USB] Probing for host controllers...\n");

    for (int i = 0; ; i++) {
        pci_device_t* dev = pci_find_class(PCI_CLASS_SERIAL_BUS, PCI_SUBCLASS_USB, i);
        if (!dev) break;
        if (num_controllers >= USB_MAX_CONTROLLERS) break;

        usb_controller_t* hc = &controllers[num_controllers];
        memset(hc, 0, sizeof(*hc));
        hc->pci = dev;

        switch (dev->prog_if) {
            case PCI_USB_PROGIF_UHCI: hc->type = USB_HC_UHCI; break;
            case PCI_USB_PROGIF_OHCI: hc->type = USB_HC_OHCI; break;
            case PCI_USB_PROGIF_EHCI: hc->type = USB_HC_EHCI; break;
            case PCI_USB_PROGIF_XHCI: hc->type = USB_HC_XHCI; break;
            default: continue;
        }

        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
        terminal_writestring("[USB] ");
        terminal_writestring(hc_type_name(hc->type));
        terminal_writestring(" controller at PCI ");
        terminal_writedec(dev->bus);
        terminal_writestring(":");
        terminal_writedec(dev->device);
        terminal_writestring(".");
        terminal_writedec(dev->function);
        terminal_writestring(" (vendor 0x");
        terminal_writehex(dev->vendor_id);
        terminal_writestring(")\n");

        int ok = 0;
        switch (hc->type) {
            case USB_HC_UHCI: ok = uhci_init(hc); break;
            case USB_HC_XHCI: ok = xhci_init(hc); break;
            default:
                terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK));
                terminal_writestring("[USB] No driver for this controller type yet\n");
                break;
        }

        if (ok) num_controllers++;
    }

    if (num_controllers == 0) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK));
        terminal_writestring("[USB] No usable controllers - keyboard pad only\n");
    }
}

void usb_poll(void) {
    for (int i = 0; i < num_controllers; i++) {
        if (controllers[i].poll)
            controllers[i].poll(&controllers[i]);
    }
}

int usb_controller_count(void) { return num_controllers; }

usb_controller_t* usb_get_controller(int index) {
    if (index < 0 || index >= num_controllers) return (usb_controller_t*)0;
    return &controllers[index];
}

/* ──────── Device announcements ──────── */

void usb_announce_device(usb_device_t* dev) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("[USB] Enumerated device: vendor 0x");
    terminal_writehex(dev->vendor_id);
    terminal_writestring(" product 0x");
    terminal_writehex(dev->product_id);
    terminal_writestring(" class ");
    terminal_writedec(dev->dev_class);
    terminal_writestring(" (addr ");
    terminal_writedec(dev->addr);
    terminal_writestring(", int-IN ep ");
    terminal_writedec(dev->ep_in);
    terminal_writestring(", ");
    terminal_writedec(dev->ep_in_maxpkt);
    terminal_writestring(" bytes)\n");

    if (dev->vendor_id == 0x054C &&
        (dev->product_id == 0x05C4 || dev->product_id == 0x09CC)) {
        terminal_writestring("[USB] DualShock 4 detected\n");
    }
}

/* ──────── HID report decoding ──────── */

static int is_dualshock4(const usb_device_t* dev) {
    return dev->vendor_id == 0x054C &&
           (dev->product_id == 0x05C4 ||    /* DualShock 4 v1 (CUH-ZCT1) */
            dev->product_id == 0x09CC);     /* DualShock 4 v2 (CUH-ZCT2) */
}

/* 0..255 axis (128 = center) → -32768..32767 */
static int16_t ds4_axis(uint8_t v) {
    return (int16_t)((int32_t)v * 257 - 32768);
}

/* The most recently active DualShock 4 (set in ds4_decode below), so
 * usb_set_rumble() has something to send an output report back to.
 * Deliberately just one device, not per-source: real-hardware testing
 * on this project has only ever involved a single physical DS4, and
 * generalizing to target a specific one of two simultaneous pads isn't
 * worth the extra bookkeeping until someone actually has two. */
static usb_controller_t* ds4_hc  = 0;
static usb_device_t*     ds4_dev = 0;

/*
 * DualShock 4 USB input report (report protocol, report ID 0x01):
 *   byte 0: 0x01 (report ID)
 *   byte 1: left stick X    byte 2: left stick Y  (0 = up)
 *   byte 3: right stick X   byte 4: right stick Y
 *   byte 5: [3:0] D-pad hat (0=N,1=NE,...,7=NW,8=released)
 *           [4] Square [5] Cross [6] Circle [7] Triangle
 *   byte 6: [0] L1 [1] R1 [2] L2 [3] R2 [4] Share [5] Options [6] L3 [7] R3
 *   byte 7: [0] PS button [1] touchpad click
 *   byte 8: L2 analog      byte 9: R2 analog
 */
static void ds4_decode(usb_controller_t* hc, usb_device_t* dev,
                       const uint8_t* d, int len) {
    if (len < 10 || d[0] != 0x01) return;

    int source = gamepad_usb_source_for_addr(dev->addr);
    if (source < 0) return;   /* Both USB pad slots already taken */

    /* Remembered so usb_set_rumble() can send an output report back to
     * this exact device later — see its comment for the "only tracks
     * the most recently active DS4" scope note. */
    ds4_hc  = hc;
    ds4_dev = dev;

    /* One-time confirmation that the input pipe is alive */
    static int announced = 0;
    if (!announced) {
        announced = 1;
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
        terminal_writestring("[USB] DualShock 4 input reports flowing\n");
    }

    uint16_t buttons = 0;

    /* D-pad hat switch */
    static const uint16_t hat_map[8] = {
        PAD_BTN_UP,
        PAD_BTN_UP | PAD_BTN_RIGHT,
        PAD_BTN_RIGHT,
        PAD_BTN_DOWN | PAD_BTN_RIGHT,
        PAD_BTN_DOWN,
        PAD_BTN_DOWN | PAD_BTN_LEFT,
        PAD_BTN_LEFT,
        PAD_BTN_UP | PAD_BTN_LEFT,
    };
    uint8_t hat = d[5] & 0x0F;
    if (hat < 8) buttons |= hat_map[hat];

    /* Face buttons (PlayStation → console layout) */
    if (d[5] & 0x20) buttons |= PAD_BTN_A;       /* Cross */
    if (d[5] & 0x40) buttons |= PAD_BTN_B;       /* Circle */
    if (d[5] & 0x10) buttons |= PAD_BTN_X;       /* Square */
    if (d[5] & 0x80) buttons |= PAD_BTN_Y;       /* Triangle */

    if (d[6] & 0x01) buttons |= PAD_BTN_L1;
    if (d[6] & 0x02) buttons |= PAD_BTN_R1;
    if (d[6] & 0x10) buttons |= PAD_BTN_SELECT;  /* Share */
    if (d[6] & 0x20) buttons |= PAD_BTN_START;   /* Options */
    if (d[6] & 0x40) buttons |= PAD_BTN_L3;
    if (d[6] & 0x80) buttons |= PAD_BTN_R3;

    gamepad_feed_usb(source, buttons,
                     ds4_axis(d[1]), ds4_axis(d[2]),
                     ds4_axis(d[3]), ds4_axis(d[4]),
                     d[8], d[9]);
}

/* DS4 USB output report (report ID 0x05). Byte layout confirmed
 * against the Linux kernel's hid-playstation.c
 * (dualshock4_output_report_common / dualshock4_output_worker) rather
 * than a half-remembered convention: byte 1 is valid_flag0, and only
 * its bit 0 (motor) and bit 1 (LED) need to be set — NOT every bit,
 * which is what the previous version did and is one real difference
 * from the reference driver, though tolerant firmware would likely
 * have accepted it either way. */
#define DS4_FLAG0_MOTOR 0x01
#define DS4_FLAG0_LED   0x02

void usb_set_rumble(uint8_t weak, uint8_t strong) {
    if (!ds4_hc || !ds4_dev) {
        /* Log once: distinguishes "never even tried" from "tried and
         * the transfer failed" the next time someone checks the log. */
        static int warned = 0;
        if (!warned) {
            warned = 1;
            terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK));
            terminal_writestring("[USB] Rumble requested but no DS4 has sent a report yet\n");
        }
        return;
    }

    uint8_t report[32];
    memset(report, 0, sizeof(report));
    report[0] = 0x05;                          /* Report ID */
    report[1] = DS4_FLAG0_MOTOR | DS4_FLAG0_LED;  /* valid_flag0 */
    report[2] = 0x00;                          /* valid_flag1 (unused here) */
    report[4] = weak;      /* motor_right (weak/high-frequency) */
    report[5] = strong;    /* motor_left (strong/low-frequency) */
    report[6] = 0x00;      /* lightbar red   */
    report[7] = 0x40;      /* lightbar green */
    report[8] = 0xFF;      /* lightbar blue — a blue glow while it's buzzing */

    usb_setup_t s;
    s.bmRequestType = 0x21;   /* Host->device, class, interface */
    s.bRequest      = 0x09;   /* SET_REPORT */
    s.wValue        = 0x0205; /* (Output report type 2 << 8) | report ID 5 */
    s.wIndex        = 0;      /* HID interface — DS4 only exposes one over USB */
    s.wLength       = sizeof(report);

    int r = -1;
    if (ds4_hc->type == USB_HC_UHCI)
        r = uhci_control_xfer(ds4_hc, ds4_dev, &s, report, sizeof(report));
    else if (ds4_hc->type == USB_HC_XHCI)
        r = xhci_control_xfer(ds4_dev, &s, report, sizeof(report));

    /* Only log on a state change — rumble fires often (every score
     * tick in a builder game), a line per call would flood the log. */
    static int last_r = -2;
    if (r != last_r) {
        last_r = r;
        terminal_setcolor(vga_entry_color(
            r >= 0 ? VGA_COLOR_LIGHT_GREEN : VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring(r >= 0 ? "[USB] Rumble SET_REPORT ok\n"
                                    : "[USB] Rumble SET_REPORT FAILED\n");
    }
}

/* ──────── USB boot-protocol keyboard ────────
 *
 * Real xHCI-only machines have no PS/2 port, so a USB keyboard must
 * drive the console. Boot-protocol reports (8 bytes: modifiers,
 * reserved, 6 key usages) are diffed against the previous report and
 * translated to set-1 scancodes injected into the SAME pipeline the
 * PS/2 keyboard uses — the two virtual pads and the ASCII buffer both
 * work identically over USB.
 */

/* HID usage → set-1 make code for the keys the console maps.
 * 0 = unmapped, 0x80 flag = 0xE0-prefixed (extended). */
static uint8_t hid_usage_to_set1(uint8_t usage, int* ext) {
    static const uint8_t letters[26] = {   /* HID 0x04..0x1D = A..Z */
        0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24,
        0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14,
        0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C,
    };
    *ext = 0;
    if (usage >= 0x04 && usage <= 0x1D) return letters[usage - 0x04];
    if (usage >= 0x1E && usage <= 0x26) return (uint8_t)(0x02 + usage - 0x1E); /* 1-9 */
    switch (usage) {
        case 0x27: return 0x0B;                    /* 0 */
        case 0x28: return 0x1C;                    /* Enter */
        case 0x29: return 0x01;                    /* Esc */
        case 0x2A: return 0x0E;                    /* Backspace */
        case 0x2B: return 0x0F;                    /* Tab */
        case 0x2C: return 0x39;                    /* Space */
        case 0x4F: *ext = 1; return 0x4D;          /* Right arrow */
        case 0x50: *ext = 1; return 0x4B;          /* Left arrow */
        case 0x51: *ext = 1; return 0x50;          /* Down arrow */
        case 0x52: *ext = 1; return 0x48;          /* Up arrow */
        default:   return 0;
    }
}

static void bootkbd_inject(uint8_t usage, int pressed) {
    int ext;
    uint8_t sc = hid_usage_to_set1(usage, &ext);
    if (!sc) return;
    if (ext) keyboard_inject_scancode(0xE0);
    keyboard_inject_scancode(pressed ? sc : (uint8_t)(sc | 0x80));
}

static void bootkbd_decode(const uint8_t* d, int len) {
    static uint8_t prev[6];
    if (len < 8) return;

    static int announced = 0;
    if (!announced) {
        announced = 1;
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
        terminal_writestring("[USB] Keyboard input reports flowing\n");
    }

    const uint8_t* cur = d + 2;
    /* Releases: in prev but not in cur */
    for (int i = 0; i < 6; i++) {
        if (!prev[i]) continue;
        int still = 0;
        for (int j = 0; j < 6; j++)
            if (cur[j] == prev[i]) { still = 1; break; }
        if (!still) bootkbd_inject(prev[i], 0);
    }
    /* Presses: in cur but not in prev */
    for (int i = 0; i < 6; i++) {
        if (!cur[i]) continue;
        int had = 0;
        for (int j = 0; j < 6; j++)
            if (prev[j] == cur[i]) { had = 1; break; }
        if (!had) bootkbd_inject(cur[i], 1);
    }
    memcpy(prev, cur, 6);
}

/*
 * Interrupt IN report sink, called by the HC drivers.
 * DualShock 4 reports are decoded; 8-byte reports from other HID
 * devices are treated as boot-protocol keyboards; anything else is
 * logged so the transfer engine can be verified with any HID device.
 */
void usb_hid_input(usb_controller_t* hc, usb_device_t* dev,
                   const uint8_t* data, int len) {
    if (is_dualshock4(dev)) {
        ds4_decode(hc, dev, data, len);
        return;
    }
    if (dev->dev_class == USB_CLASS_HID && len == 8) {
        bootkbd_decode(data, len);
        return;
    }

    /* Unknown HID device: log the first few reports for diagnostics */
    static int logged = 0;
    if (logged < 4) {
        logged++;
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
        terminal_writestring("[USB] HID report from 0x");
        terminal_writehex(dev->vendor_id);
        terminal_writestring(" (");
        terminal_writedec((uint32_t)len);
        terminal_writestring(" bytes):");
        for (int i = 0; i < len && i < 8; i++) {
            terminal_writestring(" ");
            terminal_writehex(data[i]);
        }
        terminal_writestring("\n");
    }
}
