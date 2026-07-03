/*
 * ArcadeOS – UHCI Host Controller Driver (USB 1.1)
 *
 * A polled (interrupt-free) UHCI driver with a working transfer engine:
 *
 *   - 1024-entry frame list; every frame points at the interrupt QH,
 *     which links to the control QH (standard skeleton)
 *   - Synchronous control transfers (SETUP/DATA/STATUS TD chains) used
 *     to enumerate devices: GET_DESCRIPTOR, SET_ADDRESS,
 *     SET_CONFIGURATION
 *   - One polled interrupt IN pipe per root port for HID input reports
 *     (gamepads). Completed reports are handed to usb_hid_input().
 *
 * QEMU's PIIX3/PIIX4 chipset exposes a UHCI controller when started
 * with -usb; full-speed devices like the DualShock 4 attach to it via
 * -device usb-host passthrough.
 *
 * All schedule memory (frame list, QHs, TDs, buffers) comes from
 * low-physical PMM pages allocated at init, before any user process
 * exists, so it stays inside the always-identity-mapped <4 MiB region
 * and is safe to touch from syscall context under any CR3.
 */

#include "usb.h"
#include "vga.h"
#include "pmm.h"
#include "clock.h"

/* ──────── UHCI I/O registers (offsets from BAR4) ──────── */
#define UHCI_USBCMD     0x00   /* Command (16-bit) */
#define UHCI_USBSTS     0x02   /* Status (16-bit) */
#define UHCI_USBINTR    0x04   /* Interrupt enable (16-bit) */
#define UHCI_FRNUM      0x06   /* Frame number (16-bit) */
#define UHCI_FRBASEADD  0x08   /* Frame list base address (32-bit) */
#define UHCI_SOFMOD     0x0C   /* Start of frame modify (8-bit) */
#define UHCI_PORTSC1    0x10   /* Port 1 status/control (16-bit) */
#define UHCI_PORTSC2    0x12   /* Port 2 status/control (16-bit) */

/* USBCMD bits */
#define UHCI_CMD_RUN      0x0001
#define UHCI_CMD_HCRESET  0x0002
#define UHCI_CMD_GRESET   0x0004
#define UHCI_CMD_CF       0x0040   /* Configure flag */
#define UHCI_CMD_MAXP     0x0080   /* 64-byte max packet for FS bandwidth reclamation */

/* PORTSC bits */
#define UHCI_PORT_CONNECT        0x0001
#define UHCI_PORT_CONNECT_CHANGE 0x0002
#define UHCI_PORT_ENABLE         0x0004
#define UHCI_PORT_LOW_SPEED      0x0100
#define UHCI_PORT_RESET          0x0200

/* ──────── Schedule data structures (hardware layout) ──────── */

/* Link pointer bits */
#define UHCI_LINK_TERMINATE 0x1
#define UHCI_LINK_QH        0x2
#define UHCI_LINK_DEPTH     0x4    /* TD link only: depth-first */

/* TD control/status bits */
#define TD_STS_ACTIVE    (1u << 23)
#define TD_STS_STALLED   (1u << 22)
#define TD_STS_DBUFERR   (1u << 21)
#define TD_STS_BABBLE    (1u << 20)
#define TD_STS_NAK       (1u << 19)
#define TD_STS_CRCTO     (1u << 18)
#define TD_STS_BITSTUFF  (1u << 17)
#define TD_STS_ERRMASK   (TD_STS_STALLED | TD_STS_DBUFERR | TD_STS_BABBLE | \
                          TD_STS_CRCTO | TD_STS_BITSTUFF)
#define TD_CTRL_IOC      (1u << 24)
#define TD_CTRL_LS       (1u << 26)
#define TD_CTRL_3ERR     (3u << 27)

/* TD token PIDs */
#define TD_PID_SETUP     0x2D
#define TD_PID_IN        0x69
#define TD_PID_OUT       0xE1

/* Token assembly: maxlen field holds (length - 1); 0x7FF means 0 bytes */
static uint32_t td_token(uint8_t pid, uint8_t addr, uint8_t ep, int toggle, uint32_t len) {
    uint32_t maxlen = (len == 0) ? 0x7FF : (len - 1) & 0x7FF;
    return (uint32_t)pid | ((uint32_t)addr << 8) | ((uint32_t)ep << 15)
         | ((uint32_t)(toggle & 1) << 19) | (maxlen << 21);
}

/* Transfer Descriptor: 16 bytes of hardware state, padded to 32 */
typedef struct {
    volatile uint32_t link;
    volatile uint32_t ctrl_status;
    volatile uint32_t token;
    volatile uint32_t buffer;
    uint32_t pad[4];
} uhci_td_t;

/* Queue Head: 8 bytes of hardware state, padded to 16 */
typedef struct {
    volatile uint32_t head_link;
    volatile uint32_t element_link;
    uint32_t pad[2];
} uhci_qh_t;

/* ──────── Schedule memory ────────
 *
 * One active transfer engine instance (QEMU exposes a single UHCI
 * controller; extend to per-hc allocations when targeting boards
 * with several).
 */
#define UHCI_MAX_CTRL_TDS 40

static usb_controller_t* engine_hc = (usb_controller_t*)0;

static uint32_t*  frame_list;         /* 1024 entries, 4 KiB page */
static uhci_qh_t* qh_int;             /* Interrupt pipe QH (runs first) */
static uhci_qh_t* qh_ctrl;            /* Control pipe QH */
static uhci_td_t* ctrl_tds;           /* TD pool for control transfers */
static uhci_td_t* int_td[2];          /* One interrupt IN TD per root port */

static uint8_t*   setup_buf;          /* 8-byte SETUP packet */
static uint8_t*   data_buf;           /* 256-byte control DATA buffer */
static uint8_t*   int_buf[2];         /* 64-byte interrupt report buffers */

static int        int_toggle[2];      /* Data toggle per interrupt pipe */
static uint8_t    next_dev_addr = 1;  /* USB address allocator */

/* ──────── Small helpers ──────── */

static void uhci_delay(uint32_t loops) {
    for (volatile uint32_t i = 0; i < loops; i++)
        ;
}

/* Millisecond wait that works in both boot and syscall context:
 * enable IRQs so the PIT keeps ticking, halt between ticks. */
static void uhci_wait_ms(uint32_t ms) {
    uint32_t target = system_ticks + ms;
    while (system_ticks < target)
        asm volatile("sti\nhlt");
}

static uint16_t uhci_read16(usb_controller_t* hc, uint16_t reg) {
    return inw((uint16_t)(hc->io_base + reg));
}

static void uhci_write16(usb_controller_t* hc, uint16_t reg, uint16_t val) {
    outw((uint16_t)(hc->io_base + reg), val);
}

static void uhci_write32(usb_controller_t* hc, uint16_t reg, uint32_t val) {
    asm volatile("outl %0, %1" : : "a"(val), "Nd"((uint16_t)(hc->io_base + reg)));
}

/* ──────── Control transfers (synchronous, polled) ──────── */

/*
 * Perform a control transfer on endpoint 0 of device 'addr'.
 * 'data' (data_len bytes) is transferred in the direction given by
 * setup->bmRequestType bit 7. Returns bytes transferred in the data
 * stage, or -1 on error/timeout.
 */
static int uhci_control(usb_controller_t* hc, uint8_t addr, int low_speed,
                        uint8_t ep0_maxpkt, const usb_setup_t* setup,
                        uint8_t* data, uint32_t data_len) {
    if (!engine_hc || hc != engine_hc) return -1;
    if (ep0_maxpkt == 0) ep0_maxpkt = 8;

    int dir_in = (setup->bmRequestType & 0x80) != 0;
    uint32_t ls_flag = low_speed ? TD_CTRL_LS : 0;

    memcpy(setup_buf, setup, sizeof(usb_setup_t));
    if (!dir_in && data_len > 0)
        memcpy(data_buf, data, data_len);

    /* Build the TD chain: SETUP → DATA (chunked) → STATUS */
    int n = 0;

    ctrl_tds[n].ctrl_status = TD_STS_ACTIVE | TD_CTRL_3ERR | ls_flag;
    ctrl_tds[n].token       = td_token(TD_PID_SETUP, addr, 0, 0, 8);
    ctrl_tds[n].buffer      = (uint32_t)setup_buf;
    n++;

    uint32_t remaining = data_len;
    uint32_t offset    = 0;
    int      toggle    = 1;
    while (remaining > 0 && n < UHCI_MAX_CTRL_TDS - 1) {
        uint32_t chunk = (remaining > ep0_maxpkt) ? ep0_maxpkt : remaining;

        ctrl_tds[n].ctrl_status = TD_STS_ACTIVE | TD_CTRL_3ERR | ls_flag;
        ctrl_tds[n].token       = td_token(dir_in ? TD_PID_IN : TD_PID_OUT,
                                           addr, 0, toggle, chunk);
        ctrl_tds[n].buffer      = (uint32_t)(data_buf + offset);
        n++;

        offset    += chunk;
        remaining -= chunk;
        toggle ^= 1;
    }

    /* STATUS stage: opposite direction, always DATA1, zero length */
    ctrl_tds[n].ctrl_status = TD_STS_ACTIVE | TD_CTRL_3ERR | TD_CTRL_IOC | ls_flag;
    ctrl_tds[n].token       = td_token(dir_in ? TD_PID_OUT : TD_PID_IN, addr, 0, 1, 0);
    ctrl_tds[n].buffer      = 0;
    n++;

    /* Link the chain (depth-first so it completes in few frames) */
    for (int i = 0; i < n - 1; i++)
        ctrl_tds[i].link = (uint32_t)&ctrl_tds[i + 1] | UHCI_LINK_DEPTH;
    ctrl_tds[n - 1].link = UHCI_LINK_TERMINATE;

    /* Hand the chain to the controller */
    qh_ctrl->element_link = (uint32_t)&ctrl_tds[0];

    /* Poll for completion (element link reaches TERMINATE) */
    uint32_t deadline = system_ticks + 500;   /* 500 ms is generous */
    while (!(qh_ctrl->element_link & UHCI_LINK_TERMINATE)) {
        /* Check the in-flight TD for a hard error */
        uint32_t elem = qh_ctrl->element_link & ~0xFu;
        if (elem) {
            uhci_td_t* td = (uhci_td_t*)elem;
            uint32_t sts = td->ctrl_status;
            if (!(sts & TD_STS_ACTIVE) && (sts & TD_STS_ERRMASK)) {
                qh_ctrl->element_link = UHCI_LINK_TERMINATE;
                return -1;
            }
        }
        if (system_ticks >= deadline) {
            qh_ctrl->element_link = UHCI_LINK_TERMINATE;
            return -1;
        }
        asm volatile("sti\nhlt");
    }

    /* Verify every TD retired cleanly and total the data stage length */
    int transferred = 0;
    for (int i = 0; i < n; i++) {
        uint32_t sts = ctrl_tds[i].ctrl_status;
        if (sts & (TD_STS_ACTIVE | TD_STS_ERRMASK))
            return -1;
        if (i > 0 && i < n - 1)
            transferred += (int)((sts + 1) & 0x7FF);
    }

    if (dir_in && transferred > 0)
        memcpy(data, data_buf, (uint32_t)transferred);

    return transferred;
}

/* Convenience wrappers */

static int usb_get_descriptor(usb_controller_t* hc, usb_device_t* dev,
                              uint8_t type, uint8_t index,
                              uint8_t* out, uint16_t len) {
    usb_setup_t setup;
    setup.bmRequestType = 0x80;
    setup.bRequest      = USB_REQ_GET_DESCRIPTOR;
    setup.wValue        = (uint16_t)((type << 8) | index);
    setup.wIndex        = 0;
    setup.wLength       = len;
    return uhci_control(hc, dev->addr, dev->low_speed, dev->ep0_maxpkt,
                        &setup, out, len);
}

static int usb_set_address(usb_controller_t* hc, usb_device_t* dev, uint8_t addr) {
    usb_setup_t setup;
    setup.bmRequestType = 0x00;
    setup.bRequest      = USB_REQ_SET_ADDRESS;
    setup.wValue        = addr;
    setup.wIndex        = 0;
    setup.wLength       = 0;
    int r = uhci_control(hc, 0, dev->low_speed, dev->ep0_maxpkt, &setup, 0, 0);
    if (r >= 0) {
        dev->addr = addr;
        uhci_wait_ms(5);   /* SET_ADDRESS recovery time */
    }
    return r;
}

static int usb_set_configuration(usb_controller_t* hc, usb_device_t* dev, uint8_t value) {
    usb_setup_t setup;
    setup.bmRequestType = 0x00;
    setup.bRequest      = USB_REQ_SET_CONFIGURATION;
    setup.wValue        = value;
    setup.wIndex        = 0;
    setup.wLength       = 0;
    return uhci_control(hc, dev->addr, dev->low_speed, dev->ep0_maxpkt, &setup, 0, 0);
}

/* ──────── Interrupt IN pipe (HID input reports) ──────── */

static void uhci_start_interrupt_pipe(usb_controller_t* hc, int port) {
    usb_device_t* dev = &hc->devices[port];
    uhci_td_t*    td  = int_td[port];

    int_toggle[port] = 0;

    td->link        = UHCI_LINK_TERMINATE;
    td->ctrl_status = TD_STS_ACTIVE | TD_CTRL_3ERR
                    | (dev->low_speed ? TD_CTRL_LS : 0);
    td->token       = td_token(TD_PID_IN, dev->addr, dev->ep_in,
                               int_toggle[port], dev->ep_in_maxpkt);
    td->buffer      = (uint32_t)int_buf[port];

    /* Only one element link on the interrupt QH: chain both port TDs.
     * (With two active HID devices the re-arm in the service routine
     * favors the lower port; fine for the single-gamepad case.) */
    qh_int->element_link = UHCI_LINK_TERMINATE;
    uhci_td_t* first = 0;
    for (int p = 0; p < hc->num_ports; p++) {
        if (!hc->devices[p].in_use) continue;
        if (!first) {
            first = int_td[p];
            qh_int->element_link = (uint32_t)first;
        } else {
            first->link = (uint32_t)int_td[p];   /* Breadth: next TD same frame */
            first = int_td[p];
        }
    }
    if (first) first->link = UHCI_LINK_TERMINATE;
}

static void uhci_service_interrupt_pipe(usb_controller_t* hc, int port) {
    usb_device_t* dev = &hc->devices[port];
    if (!dev->in_use) return;

    uhci_td_t* td  = int_td[port];
    uint32_t   sts = td->ctrl_status;

    if (sts & TD_STS_ACTIVE)
        return;   /* Still waiting (NAKs keep it active) */

    if (sts & TD_STS_ERRMASK) {
        /* One-time diagnostic so pipe failures are visible in the log */
        static int err_logged = 0;
        if (!err_logged) {
            err_logged = 1;
            terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
            terminal_writestring("[UHCI] Interrupt pipe error, TD status 0x");
            terminal_writehex(sts);
            terminal_writestring((sts & TD_STS_STALLED)
                ? " (STALL - host OS may still own the interface)\n"
                : "\n");
        }
        /* Endpoint error (e.g. device unplugged mid-transfer): retry */
        td->ctrl_status = TD_STS_ACTIVE | TD_CTRL_3ERR
                        | (dev->low_speed ? TD_CTRL_LS : 0);
        qh_int->element_link = (uint32_t)int_td[port];
        return;
    }

    /* A report arrived */
    int len = (int)((sts + 1) & 0x7FF);
    if (len > 0)
        usb_hid_input(dev, int_buf[port], len);

    /* Re-arm with the next data toggle */
    int_toggle[port] ^= 1;
    td->token       = td_token(TD_PID_IN, dev->addr, dev->ep_in,
                               int_toggle[port], dev->ep_in_maxpkt);
    td->ctrl_status = TD_STS_ACTIVE | TD_CTRL_3ERR
                    | (dev->low_speed ? TD_CTRL_LS : 0);
    qh_int->element_link = (uint32_t)int_td[port];
}

/* ──────── Enumeration ──────── */

static int uhci_enumerate_port(usb_controller_t* hc, int port) {
    usb_device_t* dev = &hc->devices[port];
    memset(dev, 0, sizeof(*dev));

    uint16_t sc = uhci_read16(hc, (uint16_t)(UHCI_PORTSC1 + port * 2));
    dev->port       = port;
    dev->low_speed  = (sc & UHCI_PORT_LOW_SPEED) ? 1 : 0;
    dev->ep0_maxpkt = 8;
    dev->addr       = 0;

    uhci_wait_ms(20);   /* Post-reset settle time */

    /* 1. First 8 bytes of the device descriptor → bMaxPacketSize0 */
    uint8_t desc[64];
    if (usb_get_descriptor(hc, dev, USB_DESC_DEVICE, 0, desc, 8) < 8) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("[UHCI] Enumeration failed: no device descriptor\n");
        return 0;
    }
    dev->ep0_maxpkt = desc[7];

    /* 2. Assign an address */
    if (usb_set_address(hc, dev, next_dev_addr) < 0) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("[UHCI] Enumeration failed: SET_ADDRESS\n");
        return 0;
    }
    next_dev_addr++;

    /* 3. Full device descriptor → vendor/product */
    if (usb_get_descriptor(hc, dev, USB_DESC_DEVICE, 0, desc, 18) < 18) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("[UHCI] Enumeration failed: full device descriptor\n");
        return 0;
    }
    dev->vendor_id  = (uint16_t)(desc[8]  | (desc[9]  << 8));
    dev->product_id = (uint16_t)(desc[10] | (desc[11] << 8));

    /* 4. Configuration descriptor (header first, then the full bundle) */
    static uint8_t cfg[256];
    if (usb_get_descriptor(hc, dev, USB_DESC_CONFIG, 0, cfg, 9) < 9) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("[UHCI] Enumeration failed: config header\n");
        return 0;
    }
    uint16_t total = (uint16_t)(cfg[2] | (cfg[3] << 8));
    if (total > sizeof(cfg)) total = sizeof(cfg);
    if (usb_get_descriptor(hc, dev, USB_DESC_CONFIG, 0, cfg, total) < total) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("[UHCI] Enumeration failed: config bundle\n");
        return 0;
    }
    uint8_t config_value = cfg[5];

    /* 5. Walk the bundle: first interface class + first interrupt IN EP */
    dev->ep_in = 0;
    uint16_t off = 0;
    while (off + 2 <= total) {
        uint8_t dlen  = cfg[off];
        uint8_t dtype = cfg[off + 1];
        if (dlen == 0) break;

        if (dtype == USB_DESC_INTERFACE && dev->dev_class == 0) {
            dev->dev_class = cfg[off + 5];   /* bInterfaceClass */
        } else if (dtype == USB_DESC_ENDPOINT && dev->ep_in == 0) {
            uint8_t ep_addr = cfg[off + 2];
            uint8_t attr    = cfg[off + 3];
            if ((ep_addr & 0x80) && (attr & 0x03) == 0x03) {   /* Interrupt IN */
                dev->ep_in        = ep_addr & 0x0F;
                dev->ep_in_maxpkt = (uint16_t)(cfg[off + 4] | (cfg[off + 5] << 8));
                if (dev->ep_in_maxpkt > 64) dev->ep_in_maxpkt = 64;
            }
        }
        off += dlen;
    }

    /* 6. Activate the configuration. Non-fatal: with usb-host
     * passthrough the host OS often has the device configured
     * already and rejects a second SET_CONFIGURATION. */
    if (usb_set_configuration(hc, dev, config_value) < 0) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK));
        terminal_writestring("[UHCI] SET_CONFIGURATION rejected (already configured?) - continuing\n");
    }

    dev->in_use = 1;
    usb_announce_device(dev);

    /* 7. Start polling its interrupt pipe */
    if (dev->ep_in)
        uhci_start_interrupt_pipe(hc, port);

    return 1;
}

/* ──────── Port management ──────── */

static void uhci_reset_port(usb_controller_t* hc, int port) {
    uint16_t reg = (uint16_t)(UHCI_PORTSC1 + port * 2);

    uint16_t sc = uhci_read16(hc, reg);
    uhci_write16(hc, reg, sc | UHCI_PORT_RESET);
    uhci_delay(500000);                              /* > 50 ms reset hold */
    uhci_write16(hc, reg, (uint16_t)(sc & ~UHCI_PORT_RESET));
    uhci_delay(100000);

    /* Enable the port and clear change bits (write-1-to-clear) */
    sc = uhci_read16(hc, reg);
    uhci_write16(hc, reg, sc | UHCI_PORT_ENABLE | UHCI_PORT_CONNECT_CHANGE);

    /* Wait for the enable to latch */
    uint32_t timeout = 100000;
    while (!(uhci_read16(hc, reg) & UHCI_PORT_ENABLE) && --timeout)
        ;
}

static void uhci_check_ports(usb_controller_t* hc, int announce) {
    for (int port = 0; port < hc->num_ports; port++) {
        uint16_t sc = uhci_read16(hc, (uint16_t)(UHCI_PORTSC1 + port * 2));
        int was = (hc->ports_connected >> port) & 1;
        int now = (sc & UHCI_PORT_CONNECT) ? 1 : 0;

        if (now && !was) {
            hc->ports_connected |= (1 << port);
            uhci_reset_port(hc, port);
            if (announce) {
                terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
                terminal_writestring("[UHCI] Device connected on port ");
                terminal_writedec((uint32_t)(port + 1));
                terminal_writestring("\n");
            }
            uhci_enumerate_port(hc, port);
        } else if (!now && was) {
            hc->ports_connected &= ~(1 << port);
            hc->devices[port].in_use = 0;
            uhci_start_interrupt_pipe(hc, port);   /* Rebuild TD chain */
            if (announce) {
                terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK));
                terminal_writestring("[UHCI] Device disconnected from port ");
                terminal_writedec((uint32_t)(port + 1));
                terminal_writestring("\n");
            }
        }
    }
}

/* ──────── Driver entry points ──────── */

int uhci_init(usb_controller_t* hc) {
    /* UHCI uses an I/O BAR – for PIIX it's BAR4, bit 0 set marks I/O space */
    uint32_t bar = hc->pci->bar[4];
    if (!(bar & 0x1)) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("[UHCI] BAR4 is not an I/O BAR - skipping\n");
        return 0;
    }
    hc->io_base = bar & ~0x3u;
    hc->num_ports = 2;   /* UHCI root hubs always expose 2 ports */

    pci_enable_device(hc->pci);

    /* Disable legacy keyboard/mouse SMI emulation (PCI reg 0xC0) */
    pci_write_config(hc->pci->bus, hc->pci->device, hc->pci->function, 0xC0, 0x8F00);

    /* Global reset: assert for >10ms, then release */
    uhci_write16(hc, UHCI_USBCMD, UHCI_CMD_GRESET);
    uhci_delay(500000);
    uhci_write16(hc, UHCI_USBCMD, 0);
    uhci_delay(100000);

    /* Host controller reset – hardware clears the bit when done */
    uhci_write16(hc, UHCI_USBCMD, UHCI_CMD_HCRESET);
    uint32_t timeout = 100000;
    while ((uhci_read16(hc, UHCI_USBCMD) & UHCI_CMD_HCRESET) && --timeout)
        ;
    if (timeout == 0) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("[UHCI] Controller reset timed out\n");
        return 0;
    }

    /*
     * Build the schedule. Three pages:
     *   page 0: frame list (1024 dwords)
     *   page 1: QHs + control TD pool + interrupt TDs
     *   page 2: transfer buffers
     */
    uint32_t fl_page   = pmm_alloc_page();
    uint32_t pool_page = pmm_alloc_page();
    uint32_t buf_page  = pmm_alloc_page();
    if (!fl_page || !pool_page || !buf_page) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("[UHCI] Out of memory for schedule\n");
        return 0;
    }

    frame_list = (uint32_t*)fl_page;
    memset((void*)fl_page,   0, 4096);
    memset((void*)pool_page, 0, 4096);
    memset((void*)buf_page,  0, 4096);

    qh_int    = (uhci_qh_t*)pool_page;
    qh_ctrl   = (uhci_qh_t*)(pool_page + 16);
    ctrl_tds  = (uhci_td_t*)(pool_page + 64);
    int_td[0] = (uhci_td_t*)(pool_page + 64 + UHCI_MAX_CTRL_TDS * sizeof(uhci_td_t));
    int_td[1] = int_td[0] + 1;

    setup_buf  = (uint8_t*)buf_page;
    data_buf   = (uint8_t*)(buf_page + 16);
    int_buf[0] = (uint8_t*)(buf_page + 512);
    int_buf[1] = (uint8_t*)(buf_page + 512 + 64);

    /* Skeleton: every frame → interrupt QH → control QH → end */
    qh_int->head_link     = (uint32_t)qh_ctrl | UHCI_LINK_QH;
    qh_int->element_link  = UHCI_LINK_TERMINATE;
    qh_ctrl->head_link    = UHCI_LINK_TERMINATE;
    qh_ctrl->element_link = UHCI_LINK_TERMINATE;
    for (int i = 0; i < 1024; i++)
        frame_list[i] = (uint32_t)qh_int | UHCI_LINK_QH;

    engine_hc = hc;

    /* Program the controller and start it */
    uhci_write16(hc, UHCI_USBINTR, 0);           /* Polled, no IRQs */
    uhci_write16(hc, UHCI_FRNUM, 0);
    uhci_write32(hc, UHCI_FRBASEADD, fl_page);
    outb((uint16_t)(hc->io_base + UHCI_SOFMOD), 64);
    uhci_write16(hc, UHCI_USBCMD, UHCI_CMD_RUN | UHCI_CMD_CF | UHCI_CMD_MAXP);

    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("[UHCI] Running at I/O 0x");
    terminal_writehex(hc->io_base);
    terminal_writestring(" (2 root ports, schedule at 0x");
    terminal_writehex(fl_page);
    terminal_writestring(")\n");

    /* Initial port scan + enumeration of already-connected devices */
    hc->ports_connected = 0;
    uhci_check_ports(hc, 1);

    hc->poll = uhci_poll;
    return 1;
}

void uhci_poll(usb_controller_t* hc) {
    /* Hot-plug detection */
    uhci_check_ports(hc, 1);

    /*
     * Retry enumeration for connected-but-unenumerated ports (throttled).
     * usb-host passthrough can attach a device whose first transfers
     * fail while the host OS hands it over.
     */
    static uint32_t last_retry = 0;
    if (system_ticks - last_retry > 2000) {
        last_retry = system_ticks;
        for (int port = 0; port < hc->num_ports; port++) {
            if ((hc->ports_connected & (1 << port)) && !hc->devices[port].in_use) {
                terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
                terminal_writestring("[UHCI] Retrying enumeration on port ");
                terminal_writedec((uint32_t)(port + 1));
                terminal_writestring("\n");
                uhci_reset_port(hc, port);
                uhci_enumerate_port(hc, port);
            }
        }
    }

    /* Harvest completed HID input reports */
    for (int port = 0; port < hc->num_ports; port++)
        uhci_service_interrupt_pipe(hc, port);
}
