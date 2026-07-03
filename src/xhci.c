/*
 * ArcadeOS – xHCI Host Controller Driver (USB 3.x) – Architectural Stub
 *
 * Real consoles ship xHCI; QEMU provides one with `-device qemu-xhci`.
 * This stub detects the controller and records its MMIO base so the
 * kernel boots identically on machines that only have xHCI. Transfer
 * support is structured but not yet implemented:
 *
 *   1. Map the MMIO BAR (bar[0], 64-bit) and read CAPLENGTH/HCSPARAMS
 *   2. Reset via USBCMD.HCRST, allocate the Device Context Base Address
 *      Array + Command Ring + Event Ring (contiguous PMM pages)
 *   3. Ring the command ring doorbell to enable slots, address devices
 *   4. Poll the event ring for transfer completions; HID gamepad
 *      reports are normalized and fed to usb_gamepad_report()
 */

#include "usb.h"
#include "vga.h"

int xhci_init(usb_controller_t* hc) {
    /* xHCI uses a 64-bit MMIO BAR at bar[0] */
    uint32_t bar_low  = hc->pci->bar[0];
    uint32_t bar_high = hc->pci->bar[1];

    if (bar_low & 0x1) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("[XHCI] BAR0 is I/O space - unsupported layout\n");
        return 0;
    }
    if (bar_high != 0) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("[XHCI] MMIO above 4 GiB - unreachable in 32-bit mode\n");
        return 0;
    }

    hc->io_base   = bar_low & ~0xFu;
    hc->num_ports = 0;    /* Read from HCSPARAMS1 once MMIO is mapped */
    hc->poll      = xhci_poll;

    pci_enable_device(hc->pci);

    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK));
    terminal_writestring("[XHCI] Controller detected at MMIO 0x");
    terminal_writehex(hc->io_base);
    terminal_writestring(" (driver stub - transfers not yet implemented)\n");

    /* Register it so it shows up in diagnostics, even without transfers */
    return 1;
}

void xhci_poll(usb_controller_t* hc) {
    (void)hc;
    /* Event ring processing goes here once implemented */
}
