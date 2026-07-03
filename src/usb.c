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
        terminal_writestring("[USB] DualShock 4 detected - mapping to pad 0\n");
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
static void ds4_decode(const uint8_t* d, int len) {
    if (len < 10 || d[0] != 0x01) return;

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

    gamepad_feed_usb(0, buttons,
                     ds4_axis(d[1]), ds4_axis(d[2]),
                     ds4_axis(d[3]), ds4_axis(d[4]),
                     d[8], d[9]);
}

/*
 * Interrupt IN report sink, called by the HC drivers.
 * DualShock 4 reports are decoded; other HID devices are counted so
 * the transfer engine can be verified with any HID device (e.g. QEMU's
 * emulated usb-kbd).
 */
void usb_hid_input(usb_device_t* dev, const uint8_t* data, int len) {
    if (is_dualshock4(dev)) {
        ds4_decode(data, len);
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
