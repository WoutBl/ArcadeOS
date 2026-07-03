/*
 * ArcadeOS – AC97 audio codec driver (PCM out only)
 *
 * Found on PCI as class 0x04 (multimedia) subclass 0x01 (audio); QEMU's
 * `-device AC97` emulates the classic Intel 82801AA. Both register banks
 * are I/O ports: BAR0 = NAM (mixer), BAR1 = NABM (bus master).
 *
 * Design: a tone is synthesized up-front as a 48 kHz stereo 16-bit
 * square wave into a contiguous DMA area, described by a Buffer
 * Descriptor List. The last descriptor carries no follow-up, so the
 * engine halts by itself when the tone ends — no interrupts, no refill
 * logic. Starting a new tone resets the PCM OUT box and refills.
 */

#include "audio.h"
#include "pci.h"
#include "pmm.h"
#include "vga.h"
#include "clock.h"

/* NAM (mixer) registers */
#define NAM_RESET        0x00
#define NAM_MASTER_VOL   0x02
#define NAM_PCM_OUT_VOL  0x18

/* NABM (bus master) registers — PCM OUT box */
#define NABM_PO_BDBAR    0x10   /* Buffer descriptor list base (dword) */
#define NABM_PO_CIV      0x14   /* Current index (byte) */
#define NABM_PO_LVI      0x15   /* Last valid index (byte) */
#define NABM_PO_SR       0x16   /* Status (word) */
#define NABM_PO_CR       0x1B   /* Control (byte) */
#define NABM_GLOB_CNT    0x2C   /* Global control (dword) */

#define PO_CR_RPBM       0x01   /* Run */
#define PO_CR_RR         0x02   /* Reset registers */

#define SAMPLE_RATE      48000
#define AMPLITUDE        9000

/* DMA budget: 1 page BDL + 32 data pages = 32 KiB x 4... (32 pages =
 * 128 KiB = 16384 stereo frames per page? no:) 32 pages of 4 KiB hold
 * 32 * 1024 stereo frames = ~680 ms at 48 kHz. */
#define DATA_PAGES       32
#define FRAMES_PER_PAGE  (4096 / 4)          /* 16-bit stereo = 4 B/frame */
#define MAX_FRAMES       (DATA_PAGES * FRAMES_PER_PAGE)

/* BDL entry: buffer pointer + count of 16-bit samples + flags */
typedef struct {
    uint32_t addr;
    uint16_t count;      /* Number of 16-bit samples (not frames!) */
    uint16_t flags;      /* Bit 15 = IOC, bit 14 = BUP (buffer underrun = ok) */
} __attribute__((packed)) ac97_bdl_entry_t;

static uint16_t nam  = 0;    /* I/O base: mixer */
static uint16_t nabm = 0;    /* I/O base: bus master */
static ac97_bdl_entry_t* bdl = 0;
static int16_t* pcm  = 0;    /* DATA_PAGES contiguous pages */
static uint64_t pcm_phys = 0;
static int present = 0;

int ac97_is_present(void) { return present; }

static inline uint16_t nam_read(uint8_t reg)              { return inw(nam + reg); }
static inline void nam_write(uint8_t reg, uint16_t v)     { outw(nam + reg, v); }

void ac97_stop(void) {
    if (!present) return;
    outb(nabm + NABM_PO_CR, 0);              /* Clear run */
    outb(nabm + NABM_PO_CR, PO_CR_RR);       /* Reset the PCM OUT box */
    uint32_t deadline = system_ticks + 100;
    while ((inb(nabm + NABM_PO_CR) & PO_CR_RR) && system_ticks < deadline)
        ;
}

void ac97_tone(uint32_t freq_hz, uint32_t ms) {
    if (!present) return;
    if (freq_hz < 20 || freq_hz > 20000) {
        ac97_stop();
        return;
    }

    ac97_stop();

    /* Synthesize the square wave: 48 kHz, stereo, 16-bit */
    uint32_t frames = (SAMPLE_RATE / 1000) * ms;
    if (frames > MAX_FRAMES) frames = MAX_FRAMES;
    uint32_t half_period = SAMPLE_RATE / (freq_hz * 2);
    if (half_period == 0) half_period = 1;

    for (uint32_t f = 0; f < frames; f++) {
        int16_t s = ((f / half_period) & 1) ? -AMPLITUDE : AMPLITUDE;
        pcm[f * 2]     = s;   /* Left */
        pcm[f * 2 + 1] = s;   /* Right */
    }

    /* Build the BDL: one entry per data page */
    uint32_t entries = (frames + FRAMES_PER_PAGE - 1) / FRAMES_PER_PAGE;
    for (uint32_t e = 0; e < entries; e++) {
        uint32_t first = e * FRAMES_PER_PAGE;
        uint32_t n = frames - first;
        if (n > FRAMES_PER_PAGE) n = FRAMES_PER_PAGE;
        bdl[e].addr  = (uint32_t)(pcm_phys + (uint64_t)first * 4);
        bdl[e].count = (uint16_t)(n * 2);            /* samples, not frames */
        bdl[e].flags = (e == entries - 1) ? 0x4000 : 0;   /* BUP on last */
    }

    outb(nabm + NABM_PO_CR, 0);
    outl(nabm + NABM_PO_BDBAR, (uint32_t)(uintptr_t)bdl);
    outb(nabm + NABM_PO_LVI, (uint8_t)(entries - 1));
    outw(nabm + NABM_PO_SR, 0x1C);                   /* Clear status bits */
    outb(nabm + NABM_PO_CR, PO_CR_RPBM);             /* Run */
}

int ac97_init(void) {
    pci_device_t* dev = pci_find_class(0x04, 0x01, 0);
    if (!dev) return 0;

    pci_enable_device(dev);
    nam  = (uint16_t)(dev->bar[0] & ~0x3u);
    nabm = (uint16_t)(dev->bar[1] & ~0x3u);
    if (!nam || !nabm) return 0;

    /* Global cold reset release, then codec register reset */
    outl(nabm + NABM_GLOB_CNT, 0x02);
    nam_write(NAM_RESET, 0);
    sleep_ms(1);

    /* Volumes: 0 attenuation on master and PCM out */
    nam_write(NAM_MASTER_VOL,  0x0000);
    nam_write(NAM_PCM_OUT_VOL, 0x0808);

    /* One page for the BDL, DATA_PAGES contiguous pages for PCM data */
    uint64_t bdl_page = pmm_alloc_page();
    pcm_phys = pmm_alloc_pages(DATA_PAGES);
    if (bdl_page == 0 || pcm_phys == 0) return 0;
    bdl = (ac97_bdl_entry_t*)(uintptr_t)bdl_page;
    pcm = (int16_t*)(uintptr_t)pcm_phys;
    memset(bdl, 0, 4096);

    present = 1;

    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("[AC97] Codec at NAM 0x");
    terminal_writehex(nam);
    terminal_writestring(" NABM 0x");
    terminal_writehex(nabm);
    terminal_writestring(" (48 kHz PCM out)\n");
    return 1;
}
