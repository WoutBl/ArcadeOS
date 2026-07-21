/*
 * ArcadeOS – xHCI Host Controller Driver (USB 3.x)
 *
 * A polled (interrupt-free) xHCI driver, the real-hardware counterpart
 * to uhci.c — most PCs built after ~2015 expose ONLY xHCI ports, so
 * this is what makes a USB keyboard or pad work outside QEMU's PIIX.
 *
 *   - Command ring + single-segment event ring, both polled
 *   - Slot enable / address / configure through the command ring
 *   - Synchronous control transfers on EP0 for enumeration
 *   - One interrupt IN endpoint per device, re-armed one TRB at a
 *     time; completed reports go to usb_hid_input() like UHCI's
 *
 * Supports one device (the first connected root port) — it's a game
 * console: one pad or one keyboard is the realistic setup. 32/64-byte
 * contexts are both handled (HCCPARAMS1.CSZ).
 *
 * All rings/contexts come from PMM pages; the PMM never hands out the
 * user-overlay window, so touching them from syscall context under any
 * CR3 is safe. Verify in QEMU with:  make run-xhci  (qemu-xhci + usb-kbd).
 */

#include "usb.h"
#include "vga.h"
#include "pmm.h"
#include "paging.h"
#include "clock.h"

/* ──────── Capability registers (offsets from MMIO base) ──────── */
#define XHCI_CAPLENGTH   0x00   /* Byte: operational register offset */
#define XHCI_HCSPARAMS1  0x04   /* MaxSlots[7:0] .. MaxPorts[31:24] */
#define XHCI_HCSPARAMS2  0x08   /* Scratchpad count in [25:21]|[31:27] */
#define XHCI_HCCPARAMS1  0x10   /* Bit 2: CSZ (64-byte contexts) */
#define XHCI_DBOFF       0x14   /* Doorbell array offset */
#define XHCI_RTSOFF      0x18   /* Runtime registers offset */

/* ──────── Operational registers (offsets from op base) ──────── */
#define XHCI_USBCMD      0x00
#define XHCI_USBSTS      0x04
#define XHCI_CRCR        0x18   /* Command ring control (64-bit) */
#define XHCI_DCBAAP      0x30   /* Device context base array (64-bit) */
#define XHCI_CONFIG      0x38
#define XHCI_PORTSC(n)   (0x400 + 0x10 * (n))   /* n = 0-based port */

#define CMD_RUN          (1u << 0)
#define CMD_HCRST        (1u << 1)
#define STS_HCH          (1u << 0)
#define STS_CNR          (1u << 11)

#define PORTSC_CCS       (1u << 0)    /* Current connect status */
#define PORTSC_PED       (1u << 1)    /* Port enabled */
#define PORTSC_PR        (1u << 4)    /* Port reset */
#define PORTSC_CSC       (1u << 17)   /* Connect status change (W1C) */
#define PORTSC_PRC       (1u << 21)   /* Port reset change (W1C) */
#define PORTSC_SPEED(sc) (((sc) >> 10) & 0xF)   /* 1 FS, 2 LS, 3 HS, 4 SS */

/* ──────── Runtime registers (interrupter 0, offsets from rt base) ──── */
#define XHCI_IMAN        0x20
#define XHCI_ERSTSZ      0x28
#define XHCI_ERSTBA      0x30
#define XHCI_ERDP        0x38

/* ──────── TRB types ──────── */
#define TRB_NORMAL        1
#define TRB_SETUP         2
#define TRB_DATA          3
#define TRB_STATUS        4
#define TRB_LINK          6
#define TRB_ENABLE_SLOT   9
#define TRB_ADDRESS_DEV   11
#define TRB_CONFIG_EP     12
#define TRB_EVAL_CTX      13
#define TRB_EV_TRANSFER   32
#define TRB_EV_CMD_DONE   33
#define TRB_EV_PORT       34

#define TRB_CYCLE         (1u << 0)
#define TRB_TOGGLE        (1u << 1)   /* Link TRB: toggle cycle */
#define TRB_IOC           (1u << 5)
#define TRB_IDT           (1u << 6)   /* Immediate data (setup stage) */
#define TRB_DIR_IN        (1u << 16)

#define CC_SUCCESS        1
#define CC_SHORT_PACKET   13

typedef struct {
    volatile uint32_t d0, d1, d2, d3;
} xhci_trb_t;

/* ──────── Ring bookkeeping ──────── */
typedef struct {
    xhci_trb_t* trbs;
    uint32_t    ntrbs;      /* Including the trailing link TRB */
    uint32_t    enq;
    uint8_t     cycle;
} xhci_ring_t;

#define CMD_RING_TRBS   64
#define EVT_RING_TRBS   128
#define XFER_RING_TRBS  64

/* ──────── Controller state (single instance, like uhci.c) ──────── */
static usb_controller_t* engine_hc = 0;

static volatile uint8_t*  mmio;      /* Capability base */
static volatile uint32_t* op;        /* Operational base */
static volatile uint32_t* rt;        /* Runtime base */
static volatile uint32_t* db;        /* Doorbell array */

static uint32_t max_ports;
static uint32_t ctx_sz;              /* 32 or 64 (HCCPARAMS1.CSZ) */

static uint64_t*   dcbaa;            /* Device context base address array */
static xhci_ring_t cmd_ring;
static xhci_trb_t* evt_ring;
static uint32_t    evt_deq;
static uint8_t     evt_cycle;
static uint64_t*   erst;             /* Event ring segment table */

/* The one supported device */
static int         dev_slot = 0;
static uint8_t*    dev_ctx;          /* Output device context */
static uint8_t*    input_ctx;
static xhci_ring_t ep0_ring;
static xhci_ring_t int_ring;
static uint8_t*    ctl_buf;          /* Control-transfer data (512 B) */
static uint8_t*    int_buf;          /* Interrupt IN report buffer */
static int         int_dci = 0;      /* DCI of the interrupt IN endpoint */
static uint16_t    int_maxpkt = 8;
static uint32_t    int_queued = 0;   /* Bytes queued on the pending TRB */

/* ──────── Small helpers ──────── */

static inline uint32_t rd(volatile uint32_t* r)               { return *r; }
static inline void     wr(volatile uint32_t* r, uint32_t v)   { *r = v; }
static inline void wr64(volatile uint32_t* r, uint64_t v) {
    r[0] = (uint32_t)v;
    r[1] = (uint32_t)(v >> 32);
}

/* Busy-wait (never hlt): a spin guard bounds it so USB bring-up can't
 * wedge the boot if timer ticks stall on a given host (see uhci_wait_ms). */
static void xhci_wait_ms(uint32_t ms) {
    asm volatile("sti");
    uint32_t target = system_ticks + ms;
    uint32_t guard  = 500000000u;
    while (system_ticks < target && --guard)
        asm volatile("pause");
}

static void ring_init(xhci_ring_t* r, void* mem, uint32_t ntrbs) {
    r->trbs  = (xhci_trb_t*)mem;
    r->ntrbs = ntrbs;
    r->enq   = 0;
    r->cycle = 1;
    memset(mem, 0, ntrbs * sizeof(xhci_trb_t));
    /* Trailing link TRB back to the start, toggling the cycle bit */
    xhci_trb_t* link = &r->trbs[ntrbs - 1];
    link->d0 = (uint32_t)(uintptr_t)r->trbs;
    link->d1 = 0;
    link->d2 = 0;
    link->d3 = (TRB_LINK << 10) | TRB_TOGGLE;   /* Cycle set when reached */
}

/* Push one TRB; returns its physical address (for event matching) */
static uint64_t ring_push(xhci_ring_t* r, uint32_t d0, uint32_t d1,
                          uint32_t d2, uint32_t d3) {
    xhci_trb_t* t = &r->trbs[r->enq];
    t->d0 = d0;
    t->d1 = d1;
    t->d2 = d2;
    t->d3 = (d3 & ~TRB_CYCLE) | r->cycle;

    r->enq++;
    if (r->enq == r->ntrbs - 1) {           /* Reached the link TRB */
        xhci_trb_t* link = &r->trbs[r->enq];
        link->d3 = (link->d3 & ~TRB_CYCLE) | r->cycle;
        r->enq = 0;
        r->cycle ^= 1;
    }
    return (uint64_t)(uintptr_t)t;
}

/* Dequeue one event TRB if present. Returns 1 and copies it out. */
static int event_poll(xhci_trb_t* out) {
    xhci_trb_t* e = &evt_ring[evt_deq];
    if ((e->d3 & TRB_CYCLE) != evt_cycle)
        return 0;

    out->d0 = e->d0; out->d1 = e->d1; out->d2 = e->d2; out->d3 = e->d3;

    evt_deq++;
    if (evt_deq == EVT_RING_TRBS) {
        evt_deq = 0;
        evt_cycle ^= 1;
    }
    /* Update ERDP (bit 3 = clear Event Handler Busy) */
    wr64(rt + XHCI_ERDP / 4,
         (uint64_t)(uintptr_t)&evt_ring[evt_deq] | (1u << 3));
    return 1;
}

static void handle_transfer_event(const xhci_trb_t* ev);

/*
 * Wait for a specific event type (optionally matching the TRB pointer).
 * Transfer events for the interrupt pipe are serviced along the way;
 * everything else (port changes etc.) is ignored. Returns completion
 * code, or -1 on timeout.
 */
static int wait_event(uint32_t type, uint64_t match_trb, xhci_trb_t* out) {
    asm volatile("sti");
    uint32_t deadline = system_ticks + 1000;
    uint32_t guard    = 500000000u;
    xhci_trb_t ev;
    while (system_ticks < deadline && guard) {
        if (!event_poll(&ev)) {
            guard--;
            asm volatile("pause");
            continue;
        }
        uint32_t ev_type = (ev.d3 >> 10) & 0x3F;
        uint64_t ptr     = (uint64_t)ev.d0 | ((uint64_t)ev.d1 << 32);

        if (ev_type == type && (match_trb == 0 || ptr == match_trb)) {
            if (out) *out = ev;
            return (int)(ev.d2 >> 24);
        }
        if (ev_type == TRB_EV_TRANSFER)
            handle_transfer_event(&ev);
        /* Port-change and other events: nothing to do in polled mode */
    }
    return -1;
}

/* Submit one command TRB and wait for its completion event.
 * Returns the completion code (slot id via out->d3 >> 24). */
static int cmd_submit(uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3,
                      xhci_trb_t* out) {
    uint64_t trb = ring_push(&cmd_ring, d0, d1, d2, d3);
    wr(db + 0, 0);                       /* Host controller doorbell */
    return wait_event(TRB_EV_CMD_DONE, trb, out);
}

/* ──────── Contexts ──────── */

static uint8_t* input_ctrl_ctx(void)   { return input_ctx; }
static uint8_t* input_slot_ctx(void)   { return input_ctx + ctx_sz; }
static uint8_t* input_ep_ctx(int dci)  { return input_ctx + ctx_sz * (1 + dci); }

/* ──────── Control transfers on EP0 ──────── */

static int xhci_control(usb_device_t* dev, const usb_setup_t* setup,
                        uint8_t* data, uint32_t len) {
    (void)dev;
    int dir_in = (setup->bmRequestType & 0x80) != 0;

    if (!dir_in && len > 0)
        memcpy(ctl_buf, data, len);

    /* Setup stage: immediate data, TRT = 3 (IN) / 2 (OUT) / 0 (none) */
    uint32_t trt = (len == 0) ? 0 : (dir_in ? 3 : 2);
    ring_push(&ep0_ring,
              (uint32_t)setup->bmRequestType | ((uint32_t)setup->bRequest << 8)
                  | ((uint32_t)setup->wValue << 16),
              (uint32_t)setup->wIndex | ((uint32_t)setup->wLength << 16),
              8,
              (TRB_SETUP << 10) | TRB_IDT | (trt << 16));

    if (len > 0)
        ring_push(&ep0_ring, (uint32_t)(uintptr_t)ctl_buf, 0, len,
                  (TRB_DATA << 10) | (dir_in ? TRB_DIR_IN : 0));

    /* Status stage: opposite direction, IOC — this is the TRB whose
     * completion we wait for */
    uint64_t status_trb =
        ring_push(&ep0_ring, 0, 0, 0,
                  (TRB_STATUS << 10) | TRB_IOC
                      | ((dir_in && len > 0) ? 0 : TRB_DIR_IN));

    wr(db + dev_slot, 1);                /* Doorbell: EP0 = DCI 1 */

    xhci_trb_t ev;
    int cc = wait_event(TRB_EV_TRANSFER, status_trb, &ev);
    if (cc != CC_SUCCESS && cc != CC_SHORT_PACKET)
        return -1;

    if (dir_in && len > 0)
        memcpy(data, ctl_buf, len);
    return (int)len;
}

static int xhci_get_descriptor(usb_device_t* dev, uint8_t type, uint8_t index,
                               uint8_t* out, uint16_t len) {
    usb_setup_t s;
    s.bmRequestType = 0x80;
    s.bRequest      = USB_REQ_GET_DESCRIPTOR;
    s.wValue        = (uint16_t)((type << 8) | index);
    s.wIndex        = 0;
    s.wLength       = len;
    return xhci_control(dev, &s, out, len);
}

/* ──────── Interrupt IN pipe ──────── */

static void int_pipe_arm(void) {
    if (!int_dci) return;
    int_queued = int_maxpkt;
    ring_push(&int_ring, (uint32_t)(uintptr_t)int_buf, 0, int_queued,
              (TRB_NORMAL << 10) | TRB_IOC);
    wr(db + dev_slot, (uint32_t)int_dci);
}

static void handle_transfer_event(const xhci_trb_t* ev) {
    if (!engine_hc || !int_dci) return;
    uint32_t ep = (ev->d3 >> 16) & 0x1F;
    if ((int)ep != int_dci) return;

    uint32_t cc = ev->d2 >> 24;
    if (cc == CC_SUCCESS || cc == CC_SHORT_PACKET) {
        uint32_t residual = ev->d2 & 0xFFFFFF;
        int len = (int)(int_queued - residual);
        if (len > 0)
            usb_hid_input(&engine_hc->devices[0], int_buf, len);
    }
    int_pipe_arm();                      /* Always keep a TRB pending */
}

/* ──────── Enumeration ──────── */

/* Default EP0 max packet by PORTSC speed id */
static uint16_t ep0_maxpkt_for_speed(uint32_t speed) {
    switch (speed) {
        case 2:  return 8;      /* LS */
        case 1:  return 8;      /* FS (may grow via descriptor) */
        case 3:  return 64;     /* HS */
        default: return 512;    /* SS+ */
    }
}

static int xhci_enumerate_port(usb_controller_t* hc, int port) {
    uint32_t sc = rd(op + XHCI_PORTSC(port) / 4);
    uint32_t speed = PORTSC_SPEED(sc);

    usb_device_t* dev = &hc->devices[0];
    memset(dev, 0, sizeof(*dev));
    dev->port       = port;
    dev->low_speed  = (speed == 2);
    dev->ep0_maxpkt = (uint8_t)((ep0_maxpkt_for_speed(speed) > 255)
                                ? 255 : ep0_maxpkt_for_speed(speed));

    /* 1. Enable Slot */
    xhci_trb_t ev;
    if (cmd_submit(0, 0, 0, TRB_ENABLE_SLOT << 10, &ev) != CC_SUCCESS) {
        terminal_writestring("[XHCI] Enable Slot failed\n");
        return 0;
    }
    dev_slot = (int)(ev.d3 >> 24);
    dcbaa[dev_slot] = (uint64_t)(uintptr_t)dev_ctx;

    /* 2. Input context: slot + EP0, then Address Device */
    memset(input_ctx, 0, ctx_sz * 34);
    uint32_t* icc = (uint32_t*)input_ctrl_ctx();
    icc[1] = 0x3;                                    /* Add slot + EP0 */

    uint32_t* slot = (uint32_t*)input_slot_ctx();
    slot[0] = (1u << 27) | (speed << 20);            /* 1 context entry */
    slot[1] = (uint32_t)(port + 1) << 16;            /* Root port (1-based) */

    uint32_t* ep0 = (uint32_t*)input_ep_ctx(1);
    ep0[1] = (4u << 3) | (3u << 1)                   /* Control EP, CErr 3 */
           | ((uint32_t)ep0_maxpkt_for_speed(speed) << 16);
    ep0[2] = (uint32_t)(uintptr_t)ep0_ring.trbs | 1; /* DCS = 1 */
    ep0[3] = 0;
    ep0[4] = 8;                                      /* Average TRB length */

    if (cmd_submit((uint32_t)(uintptr_t)input_ctx, 0, 0,
                   (TRB_ADDRESS_DEV << 10) | ((uint32_t)dev_slot << 24),
                   &ev) != CC_SUCCESS) {
        terminal_writestring("[XHCI] Address Device failed\n");
        return 0;
    }
    dev->addr = 1;                                   /* xHC assigned it */

    /* 3. Device descriptor → IDs (and real EP0 max packet for FS) */
    uint8_t desc[18];
    if (xhci_get_descriptor(dev, USB_DESC_DEVICE, 0, desc, 8) < 0) {
        terminal_writestring("[XHCI] No device descriptor\n");
        return 0;
    }
    if (speed <= 1 && desc[7] != ep0_maxpkt_for_speed(speed)) {
        /* FS device with a bigger EP0: Evaluate Context with new size */
        memset(input_ctx, 0, ctx_sz * 34);
        icc[1] = 0x2;                                /* Add EP0 only */
        ep0 = (uint32_t*)input_ep_ctx(1);
        ep0[1] = (4u << 3) | (3u << 1) | ((uint32_t)desc[7] << 16);
        cmd_submit((uint32_t)(uintptr_t)input_ctx, 0, 0,
                   (TRB_EVAL_CTX << 10) | ((uint32_t)dev_slot << 24), &ev);
        dev->ep0_maxpkt = desc[7];
    }
    if (xhci_get_descriptor(dev, USB_DESC_DEVICE, 0, desc, 18) < 0)
        return 0;
    dev->vendor_id  = (uint16_t)(desc[8]  | (desc[9]  << 8));
    dev->product_id = (uint16_t)(desc[10] | (desc[11] << 8));

    /* 4. Config descriptor → interface class, interrupt IN endpoint */
    static uint8_t cfg[256];
    if (xhci_get_descriptor(dev, USB_DESC_CONFIG, 0, cfg, 9) < 0)
        return 0;
    uint16_t total = (uint16_t)(cfg[2] | (cfg[3] << 8));
    if (total > sizeof(cfg)) total = sizeof(cfg);
    if (xhci_get_descriptor(dev, USB_DESC_CONFIG, 0, cfg, total) < 0)
        return 0;
    uint8_t config_value = cfg[5];

    uint8_t  ep_addr = 0, ep_interval = 8;
    uint16_t off = 0;
    while (off + 2 <= total) {
        uint8_t dlen  = cfg[off];
        uint8_t dtype = cfg[off + 1];
        if (dlen == 0) break;
        if (dtype == USB_DESC_INTERFACE && dev->dev_class == 0) {
            dev->dev_class = cfg[off + 5];
        } else if (dtype == USB_DESC_ENDPOINT && ep_addr == 0) {
            if ((cfg[off + 2] & 0x80) && (cfg[off + 3] & 0x03) == 0x03) {
                ep_addr       = cfg[off + 2];
                dev->ep_in    = ep_addr & 0x0F;
                dev->ep_in_maxpkt = (uint16_t)(cfg[off + 4] | (cfg[off + 5] << 8));
                if (dev->ep_in_maxpkt > 64) dev->ep_in_maxpkt = 64;
                ep_interval   = cfg[off + 6];
            }
        }
        off += dlen;
    }

    /* 5. Configure the interrupt IN endpoint */
    if (ep_addr) {
        int_dci    = dev->ep_in * 2 + 1;
        int_maxpkt = dev->ep_in_maxpkt;

        /* xHCI interval is log2(microframes): FS/LS bInterval is in
         * ms (frames), HS+ is 2^(bInterval-1) microframes already */
        uint32_t interval;
        if (speed == 1 || speed == 2) {
            uint32_t uframes = (uint32_t)ep_interval * 8;
            interval = 3;
            while ((1u << interval) < uframes && interval < 15) interval++;
        } else {
            interval = (ep_interval > 0) ? (uint32_t)(ep_interval - 1) : 0;
        }

        memset(input_ctx, 0, ctx_sz * 34);
        icc[1] = 0x1 | (1u << int_dci);              /* Slot + the EP */

        slot = (uint32_t*)input_slot_ctx();
        slot[0] = ((uint32_t)int_dci << 27) | (speed << 20);
        slot[1] = (uint32_t)(port + 1) << 16;

        uint32_t* epc = (uint32_t*)input_ep_ctx(int_dci);
        epc[0] = interval << 16;
        epc[1] = (7u << 3) | (3u << 1)               /* Interrupt IN, CErr 3 */
               | ((uint32_t)int_maxpkt << 16);
        epc[2] = (uint32_t)(uintptr_t)int_ring.trbs | 1;
        epc[3] = 0;
        epc[4] = (uint32_t)int_maxpkt | ((uint32_t)int_maxpkt << 16);

        if (cmd_submit((uint32_t)(uintptr_t)input_ctx, 0, 0,
                       (TRB_CONFIG_EP << 10) | ((uint32_t)dev_slot << 24),
                       &ev) != CC_SUCCESS) {
            terminal_writestring("[XHCI] Configure Endpoint failed\n");
            int_dci = 0;
        }
    }

    /* 6. SET_CONFIGURATION */
    usb_setup_t s;
    s.bmRequestType = 0x00;
    s.bRequest      = USB_REQ_SET_CONFIGURATION;
    s.wValue        = config_value;
    s.wIndex        = 0;
    s.wLength       = 0;
    xhci_control(dev, &s, 0, 0);

    dev->in_use = 1;
    usb_announce_device(dev);

    /* 7. Start the interrupt pipe */
    if (int_dci)
        int_pipe_arm();

    return 1;
}

/* ──────── Port management ──────── */

static void xhci_check_ports(usb_controller_t* hc) {
    if (hc->devices[0].in_use) {
        /* Device gone? (CCS dropped on its port) */
        uint32_t sc = rd(op + XHCI_PORTSC(hc->devices[0].port) / 4);
        if (!(sc & PORTSC_CCS)) {
            terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK));
            terminal_writestring("[XHCI] Device disconnected\n");
            hc->devices[0].in_use = 0;
            int_dci = 0;
        }
        return;
    }

    for (uint32_t p = 0; p < max_ports; p++) {
        uint32_t sc = rd(op + XHCI_PORTSC(p) / 4);
        if (!(sc & PORTSC_CCS)) continue;

        /* USB2 ports need an explicit reset; USB3 ports self-enable.
         * Write PR only if the port isn't enabled yet. Preserve CCS-
         * adjacent W1C bits by not writing them back as set. */
        if (!(sc & PORTSC_PED)) {
            wr(op + XHCI_PORTSC(p) / 4, (sc & 0x0E00C3E0u) | PORTSC_PR);
            asm volatile("sti");
            uint32_t deadline = system_ticks + 200;
            uint32_t guard    = 500000000u;
            while (system_ticks < deadline && --guard) {
                sc = rd(op + XHCI_PORTSC(p) / 4);
                if (sc & PORTSC_PRC) break;
                asm volatile("pause");
            }
            wr(op + XHCI_PORTSC(p) / 4,
               (sc & 0x0E00C3E0u) | PORTSC_PRC | PORTSC_CSC);
            xhci_wait_ms(10);
            sc = rd(op + XHCI_PORTSC(p) / 4);
        }
        if (!(sc & PORTSC_PED)) continue;

        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
        terminal_writestring("[XHCI] Device connected on port ");
        terminal_writedec(p + 1);
        terminal_writestring("\n");
        xhci_enumerate_port(hc, (int)p);
        return;                          /* One device supported */
    }
}

/* ──────── Driver entry points ──────── */

int xhci_init(usb_controller_t* hc) {
    uint32_t bar_low  = hc->pci->bar[0];
    uint32_t bar_high = hc->pci->bar[1];

    if (bar_low & 0x1) {
        terminal_writestring("[XHCI] BAR0 is I/O space - unsupported layout\n");
        return 0;
    }
    uint64_t mmio_phys = ((uint64_t)bar_high << 32) | (bar_low & ~0xFull);

    pci_enable_device(hc->pci);
    paging_kernel_map_mmio(mmio_phys, 0x10000);   /* 64 KiB register window */

    mmio = (volatile uint8_t*)(uintptr_t)mmio_phys;
    uint32_t caplen  = mmio[XHCI_CAPLENGTH];
    uint32_t hcs1    = *(volatile uint32_t*)(mmio + XHCI_HCSPARAMS1);
    uint32_t hcs2    = *(volatile uint32_t*)(mmio + XHCI_HCSPARAMS2);
    uint32_t hcc1    = *(volatile uint32_t*)(mmio + XHCI_HCCPARAMS1);
    uint32_t dboff   = *(volatile uint32_t*)(mmio + XHCI_DBOFF)  & ~0x3u;
    uint32_t rtsoff  = *(volatile uint32_t*)(mmio + XHCI_RTSOFF) & ~0x1Fu;

    op = (volatile uint32_t*)(mmio + caplen);
    rt = (volatile uint32_t*)(mmio + rtsoff);
    db = (volatile uint32_t*)(mmio + dboff);

    uint32_t max_slots = hcs1 & 0xFF;
    max_ports = (hcs1 >> 24) & 0xFF;
    ctx_sz    = (hcc1 & (1u << 2)) ? 64 : 32;

    hc->io_base   = (uint32_t)mmio_phys;
    hc->num_ports = (int)max_ports;

    /* Halt + reset the controller */
    wr(op + XHCI_USBCMD / 4, rd(op + XHCI_USBCMD / 4) & ~CMD_RUN);
    /* Spin guards (in addition to the tick deadline) so a stalled timer
     * can't hang controller reset on real hardware. */
    uint32_t deadline = system_ticks + 100;
    uint32_t g = 500000000u;
    while (!(rd(op + XHCI_USBSTS / 4) & STS_HCH) && system_ticks < deadline && --g)
        ;
    wr(op + XHCI_USBCMD / 4, CMD_HCRST);
    deadline = system_ticks + 500; g = 500000000u;
    while ((rd(op + XHCI_USBCMD / 4) & CMD_HCRST) && system_ticks < deadline && --g)
        ;
    g = 500000000u;
    while ((rd(op + XHCI_USBSTS / 4) & STS_CNR) && system_ticks < deadline && --g)
        ;
    if (rd(op + XHCI_USBCMD / 4) & CMD_HCRST) {
        terminal_writestring("[XHCI] Controller reset timed out\n");
        return 0;
    }

    /*
     * Memory: 5 PMM pages.
     *   0: DCBAA (+ scratchpad array at +2048 if the HC wants any)
     *   1: command ring (1 KiB) + event ring (2 KiB) + ERST (64 B)
     *   2: input context + device context
     *   3: EP0 ring (1 KiB) + interrupt ring (1 KiB) + buffers
     *   4: scratchpad page(s) — QEMU asks for 0; real HCs a handful
     */
    uint64_t pg[4];
    for (int i = 0; i < 4; i++) {
        pg[i] = pmm_alloc_page();
        if (!pg[i]) return 0;
        memset((void*)(uintptr_t)pg[i], 0, 4096);
    }

    dcbaa = (uint64_t*)(uintptr_t)pg[0];
    ring_init(&cmd_ring, (void*)(uintptr_t)pg[1], CMD_RING_TRBS);
    evt_ring = (xhci_trb_t*)(uintptr_t)(pg[1] + 1024);
    erst     = (uint64_t*)(uintptr_t)(pg[1] + 1024 + EVT_RING_TRBS * 16);
    evt_deq   = 0;
    evt_cycle = 1;

    input_ctx = (uint8_t*)(uintptr_t)pg[2];
    dev_ctx   = (uint8_t*)(uintptr_t)(pg[2] + 2176);   /* 34 * 64 max */
    ring_init(&ep0_ring, (void*)(uintptr_t)pg[3], XFER_RING_TRBS);
    ring_init(&int_ring, (void*)(uintptr_t)(pg[3] + 1024), XFER_RING_TRBS);
    ctl_buf = (uint8_t*)(uintptr_t)(pg[3] + 2048);
    int_buf = (uint8_t*)(uintptr_t)(pg[3] + 2048 + 512);

    /* Scratchpad buffers, if the controller demands them */
    uint32_t n_scratch = (((hcs2 >> 21) & 0x1F) << 5) | ((hcs2 >> 27) & 0x1F);
    if (n_scratch > 0) {
        uint64_t* sp_array = (uint64_t*)(uintptr_t)(pg[0] + 2048);
        if (n_scratch > 256) n_scratch = 256;
        for (uint32_t i = 0; i < n_scratch; i++) {
            uint64_t sp = pmm_alloc_page();
            if (!sp) return 0;
            memset((void*)(uintptr_t)sp, 0, 4096);
            sp_array[i] = sp;
        }
        dcbaa[0] = (uint64_t)(uintptr_t)sp_array;
    }

    /* Program the controller */
    wr(op + XHCI_CONFIG / 4, max_slots);
    wr64(op + XHCI_DCBAAP / 4, (uint64_t)(uintptr_t)dcbaa);
    wr64(op + XHCI_CRCR / 4, (uint64_t)(uintptr_t)cmd_ring.trbs | 1);

    erst[0] = (uint64_t)(uintptr_t)evt_ring;         /* Segment base */
    erst[1] = EVT_RING_TRBS;                         /* Segment size */
    wr(rt + XHCI_ERSTSZ / 4, 1);
    wr64(rt + XHCI_ERDP / 4, (uint64_t)(uintptr_t)evt_ring);
    wr64(rt + XHCI_ERSTBA / 4, (uint64_t)(uintptr_t)erst);

    wr(op + XHCI_USBCMD / 4, CMD_RUN);               /* Polled: no INTE */
    xhci_wait_ms(50);                                /* Port power settle */

    engine_hc = hc;
    hc->poll  = xhci_poll;

    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("[XHCI] Running at MMIO 0x");
    terminal_writehex(mmio_phys);
    terminal_writestring(" (");
    terminal_writedec(max_ports);
    terminal_writestring(" ports, ");
    terminal_writedec(ctx_sz);
    terminal_writestring("-byte contexts)\n");

    xhci_check_ports(hc);
    return 1;
}

void xhci_poll(usb_controller_t* hc) {
    if (hc != engine_hc) return;

    /* Drain the event ring: interrupt-pipe completions + anything else */
    xhci_trb_t ev;
    while (event_poll(&ev)) {
        uint32_t type = (ev.d3 >> 10) & 0x3F;
        if (type == TRB_EV_TRANSFER)
            handle_transfer_event(&ev);
    }

    /* Hot-plug: throttle the PORTSC scan to every ~500 ms */
    static uint32_t last_scan = 0;
    if (system_ticks - last_scan > 500) {
        last_scan = system_ticks;
        xhci_check_ports(hc);
    }
}
