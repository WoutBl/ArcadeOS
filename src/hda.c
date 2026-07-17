/*
 * ArcadeOS – Intel HD Audio controller driver (PCM out only)
 *
 * The real-hardware audio backend: post-~2008 PCs expose HDA, not
 * AC97. Found on PCI as class 0x04 subclass 0x03; QEMU emulates it
 * with `-device intel-hda -device hda-output`.
 *
 * Same transport model as the AC97 driver: an ALWAYS-RUNNING cyclic
 * stream over a 32-page BDL, with hda_tick() rendering freshly mixed
 * audio (mixer_render) a fixed distance ahead of the hardware position
 * — no stream open/close per sound, no busy-waits in syscalls.
 *
 * Codec setup is done generically over CORB/RIRB: walk the function
 * groups for the first audio-out converter (DAC) and pin complex,
 * wire stream 1 into the DAC, power/unmute/enable the pin. That works
 * on QEMU's codecs and should survive first contact with real ones.
 */

#include "audio.h"
#include "pci.h"
#include "pmm.h"
#include "paging.h"
#include "vga.h"
#include "clock.h"

/* ──────── Controller registers (MMIO, offsets from BAR0) ──────── */
#define REG_GCAP        0x00
#define REG_GCTL        0x08
#define REG_STATESTS    0x0E
#define REG_CORBLBASE   0x40
#define REG_CORBUBASE   0x44
#define REG_CORBWP      0x48
#define REG_CORBRP      0x4A
#define REG_CORBCTL     0x4C
#define REG_CORBSIZE    0x4E
#define REG_RIRBLBASE   0x50
#define REG_RIRBUBASE   0x54
#define REG_RIRBWP      0x58
#define REG_RINTCNT     0x5A
#define REG_RIRBCTL     0x5C
#define REG_RIRBSTS     0x5D
#define REG_RIRBSIZE    0x5E
#define REG_SD_BASE     0x80    /* Stream descriptors, 0x20 apiece */

/* Stream-descriptor register offsets */
#define SD_CTL          0x00    /* 24-bit; bit0 SRST, bit1 RUN */
#define SD_LPIB         0x04
#define SD_CBL          0x08
#define SD_LVI          0x0C
#define SD_FMT          0x12
#define SD_BDPL         0x18
#define SD_BDPU         0x1C

/* ──────── Ring geometry (matches the AC97 transport) ──────── */
#define SAMPLE_RATE      48000
#define RING_PAGES       32
#define FRAMES_PER_PAGE  (4096 / 4)          /* 16-bit stereo */
#define LOOKAHEAD_PAGES  2

/* 48 kHz, 16-bit, stereo */
#define STREAM_FMT       0x0011
#define STREAM_ID        1

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint32_t flags;
} __attribute__((packed)) bdl_entry_t;

static volatile uint8_t* mmio = 0;
static int      present = 0;
static uint32_t sd_off;              /* First OUTPUT stream descriptor */
static int      cad = 0;             /* Codec address */

static uint32_t* corb;               /* 256 x u32 */
static uint64_t* rirb;               /* 256 x u64 */
static bdl_entry_t* bdl;
static int16_t*  pcm;
static uint64_t  pcm_phys;

static uint16_t corb_wp = 0;
static uint16_t rirb_rp = 0;
static uint8_t  render_page = 0;

int hda_is_present(void) { return present; }

static inline uint8_t  r8(uint32_t o)  { return mmio[o]; }
static inline uint16_t r16(uint32_t o) { return *(volatile uint16_t*)(mmio + o); }
static inline uint32_t r32(uint32_t o) { return *(volatile uint32_t*)(mmio + o); }
static inline void w8(uint32_t o, uint8_t v)   { mmio[o] = v; }
static inline void w16(uint32_t o, uint16_t v) { *(volatile uint16_t*)(mmio + o) = v; }
static inline void w32(uint32_t o, uint32_t v) { *(volatile uint32_t*)(mmio + o) = v; }

/* Send one verb over CORB, wait for its RIRB response (polled) */
static uint32_t verb(uint32_t nid, uint32_t v, uint32_t payload) {
    uint32_t cmd = ((uint32_t)cad << 28) | (nid << 20);
    if (v >= 0x100) cmd |= (v << 8) | (payload & 0xFF);      /* 12-bit verb */
    else            cmd |= (v << 16) | (payload & 0xFFFF);   /* 4-bit verb */

    corb_wp = (uint16_t)((corb_wp + 1) & 0xFF);
    corb[corb_wp] = cmd;
    w16(REG_CORBWP, corb_wp);

    uint32_t deadline = system_ticks + 50;
    while ((r16(REG_RIRBWP) & 0xFF) == ((rirb_rp) & 0xFF)) {
        if (system_ticks > deadline) return 0xFFFFFFFFu;
    }
    rirb_rp = (uint16_t)((rirb_rp + 1) & 0xFF);
    uint32_t resp = (uint32_t)rirb[rirb_rp];
    w8(REG_RIRBSTS, 0x5);       /* Ack: the CORB engine stalls until the
                                 * response status is cleared */
    return resp;
}

#define GET_PARAM 0xF00

/* Walk the codec for the first audio-out converter + pin complex */
static int codec_setup(void) {
    uint32_t sub = verb(0, GET_PARAM, 0x04);      /* Root: FG range */
    uint32_t fg_start = (sub >> 16) & 0xFF, fg_count = sub & 0xFF;

    for (uint32_t fg = fg_start; fg < fg_start + fg_count; fg++) {
        if ((verb(fg, GET_PARAM, 0x05) & 0x7F) != 0x01)
            continue;                             /* Audio FGs only */
        verb(fg, 0x705, 0x00);                    /* FG to D0 */

        uint32_t wr = verb(fg, GET_PARAM, 0x04);  /* Widget range */
        uint32_t w_start = (wr >> 16) & 0xFF, w_count = wr & 0xFF;
        uint32_t dac = 0, pin = 0;

        for (uint32_t w = w_start; w < w_start + w_count; w++) {
            uint32_t caps = verb(w, GET_PARAM, 0x09);
            uint32_t type = (caps >> 20) & 0xF;
            if (type == 0x0 && !dac) dac = w;     /* Audio output */
            if (type == 0x4 && !pin) {            /* Pin complex */
                /* Only pins that can drive an output */
                if (verb(w, GET_PARAM, 0x0C) & (1u << 4))
                    if (!pin) pin = w;
            }
        }
        if (!dac || !pin) continue;

        verb(dac, 0x705, 0x00);                   /* Power */
        verb(pin, 0x705, 0x00);
        verb(dac, 0x200, STREAM_FMT);             /* Converter format */
        verb(dac, 0x706, STREAM_ID << 4);         /* Stream/channel */
        verb(dac, 0x300, 0xB000);                 /* Unmute output amp */
        verb(pin, 0x300, 0xB000);
        verb(pin, 0x707, 0xC0);                   /* Pin: out + HP enable */
        verb(pin, 0x70C, 0x02);                   /* EAPD on */

        terminal_writestring("[HDA] Codec ");
        terminal_writedec((uint32_t)cad);
        terminal_writestring(": DAC nid ");
        terminal_writedec(dac);
        terminal_writestring(", pin nid ");
        terminal_writedec(pin);
        terminal_writestring("\n");
        return 1;
    }
    return 0;
}

/* PIT tick: keep the cyclic stream freshly mixed (see ac97_tick) */
void hda_tick(void) {
    if (!present) return;

    uint32_t lpib = r32(sd_off + SD_LPIB);
    uint8_t  hw_page = (uint8_t)((lpib / 4096) % RING_PAGES);

    uint8_t target = (uint8_t)((hw_page + 1 + LOOKAHEAD_PAGES) % RING_PAGES);
    while (render_page != target) {
        mixer_render(pcm + (uint32_t)render_page * FRAMES_PER_PAGE * 2,
                     FRAMES_PER_PAGE);
        render_page = (uint8_t)((render_page + 1) % RING_PAGES);
    }
}

int hda_init(void) {
    pci_device_t* dev = pci_find_class(0x04, 0x03, 0);
    if (!dev) return 0;

    pci_enable_device(dev);
    uint64_t bar = dev->bar[0] & ~0xFULL;
    if (!bar) return 0;
    paging_kernel_map_mmio(bar, 0x4000);
    mmio = (volatile uint8_t*)(uintptr_t)bar;

    /* Controller reset: CRST 0 → 1 */
    w32(REG_GCTL, 0);
    uint32_t deadline = system_ticks + 100;
    while ((r32(REG_GCTL) & 1) && system_ticks < deadline) ;
    w32(REG_GCTL, 1);
    deadline = system_ticks + 100;
    while (!(r32(REG_GCTL) & 1) && system_ticks < deadline) ;
    sleep_ms(2);                          /* Codecs announce themselves */

    uint16_t codecs = r16(REG_STATESTS);
    if (!codecs) return 0;
    for (cad = 0; cad < 15 && !(codecs & (1 << cad)); cad++) ;

    /* First OUTPUT stream descriptor comes after the input ones */
    uint16_t gcap = r16(REG_GCAP);
    uint32_t iss = (gcap >> 8) & 0xF;
    sd_off = REG_SD_BASE + iss * 0x20;

    /* CORB/RIRB: one page each, 256 entries */
    uint64_t corb_page = pmm_alloc_page();
    uint64_t rirb_page = pmm_alloc_page();
    uint64_t bdl_page  = pmm_alloc_page();
    pcm_phys = pmm_alloc_pages(RING_PAGES);
    if (!corb_page || !rirb_page || !bdl_page || !pcm_phys) return 0;
    corb = (uint32_t*)(uintptr_t)corb_page;
    rirb = (uint64_t*)(uintptr_t)rirb_page;
    bdl  = (bdl_entry_t*)(uintptr_t)bdl_page;
    pcm  = (int16_t*)(uintptr_t)pcm_phys;
    memset(corb, 0, 4096);
    memset(rirb, 0, 4096);
    memset(bdl, 0, 4096);
    memset(pcm, 0, RING_PAGES * 4096);

    /* CORB up */
    w8(REG_CORBCTL, 0);
    w32(REG_CORBLBASE, (uint32_t)corb_page);
    w32(REG_CORBUBASE, 0);
    w8(REG_CORBSIZE, 0x2);                /* 256 entries */
    w16(REG_CORBRP, 0x8000);              /* Reset read pointer */
    w16(REG_CORBRP, 0);
    w16(REG_CORBWP, 0);
    corb_wp = 0;
    w8(REG_CORBCTL, 0x02);                /* DMA run */

    /* RIRB up */
    w8(REG_RIRBCTL, 0);
    w32(REG_RIRBLBASE, (uint32_t)rirb_page);
    w32(REG_RIRBUBASE, 0);
    w8(REG_RIRBSIZE, 0x2);
    w16(REG_RIRBWP, 0x8000);              /* Reset write pointer */
    w16(REG_RINTCNT, 255);      /* Max batching before a forced stall */
    rirb_rp = 0;
    w8(REG_RIRBCTL, 0x02);                /* DMA run */

    if (!codec_setup()) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK));
        terminal_writestring("[HDA] No usable output path on codec\n");
        return 0;
    }

    /* Cyclic BDL over the PCM ring */
    for (int i = 0; i < RING_PAGES; i++) {
        bdl[i].addr  = pcm_phys + (uint64_t)i * 4096;
        bdl[i].len   = 4096;
        bdl[i].flags = 0;
    }

    /* Output stream: reset, program, run */
    w8(sd_off + SD_CTL, 0x01);            /* SRST */
    deadline = system_ticks + 50;
    while (!(r8(sd_off + SD_CTL) & 1) && system_ticks < deadline) ;
    w8(sd_off + SD_CTL, 0x00);
    deadline = system_ticks + 50;
    while ((r8(sd_off + SD_CTL) & 1) && system_ticks < deadline) ;

    w32(sd_off + SD_CBL, RING_PAGES * 4096);
    w16(sd_off + SD_LVI, RING_PAGES - 1);
    w16(sd_off + SD_FMT, STREAM_FMT);
    w32(sd_off + SD_BDPL, (uint32_t)bdl_page);
    w32(sd_off + SD_BDPU, 0);
    w8(sd_off + SD_CTL + 2, STREAM_ID << 4);   /* Stream tag */
    w8(sd_off + SD_CTL, 0x02);            /* RUN */

    render_page = 1;
    present = 1;

    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("[HDA] Controller at 0x");
    terminal_writehex(bar);
    terminal_writestring(" (48 kHz stream, always running)\n");
    return 1;
}
