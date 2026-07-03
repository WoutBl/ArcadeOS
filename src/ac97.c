/*
 * ArcadeOS – AC97 audio codec driver (PCM out only)
 *
 * Found on PCI as class 0x04 (multimedia) subclass 0x01 (audio); QEMU's
 * `-device AC97` emulates the classic Intel 82801AA. Both register banks
 * are I/O ports: BAR0 = NAM (mixer), BAR1 = NABM (bus master).
 *
 * Design: an ALWAYS-RUNNING ring. The DMA engine is started once at init
 * on a 32-page ring of silence and never stopped: the PIT tick keeps the
 * Last Valid Index one buffer behind the hardware's Current Index (so
 * the engine never halts) and re-silences pages the hardware finished.
 * Playing a tone just writes square-wave samples into the ring ahead of
 * the hardware position — no stream open/close per sound, which host
 * audio backends (e.g. macOS coreaudio) punish with glitches and stalls,
 * and no busy-waits in the syscall path.
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
#define NABM_PO_CIV      0x14   /* Current index (byte, read-only) */
#define NABM_PO_LVI      0x15   /* Last valid index (byte) */
#define NABM_PO_SR       0x16   /* Status (word) */
#define NABM_PO_CR       0x1B   /* Control (byte) */
#define NABM_GLOB_CNT    0x2C   /* Global control (dword) */

#define PO_CR_RPBM       0x01   /* Run */
#define PO_CR_RR         0x02   /* Reset registers */

#define SAMPLE_RATE      48000
#define AMPLITUDE        9000

/* Ring geometry: 32 pages x 1024 stereo frames = ~680 ms of audio */
#define RING_PAGES       32
#define FRAMES_PER_PAGE  (4096 / 4)          /* 16-bit stereo = 4 B/frame */
#define RING_FRAMES      (RING_PAGES * FRAMES_PER_PAGE)

/* BDL entry: buffer pointer + count of 16-bit samples + flags */
typedef struct {
    uint32_t addr;
    uint16_t count;      /* Number of 16-bit samples (not frames!) */
    uint16_t flags;
} __attribute__((packed)) ac97_bdl_entry_t;

static uint16_t nam  = 0;    /* I/O base: mixer */
static uint16_t nabm = 0;    /* I/O base: bus master */
static ac97_bdl_entry_t* bdl = 0;
static int16_t* pcm  = 0;    /* RING_PAGES contiguous pages */
static uint64_t pcm_phys = 0;
static int present = 0;
static uint8_t prev_civ = 0;

int ac97_is_present(void) { return present; }

static inline void nam_write(uint8_t reg, uint16_t v) { outw(nam + reg, v); }

/* Zero a whole ring page (one BDL buffer worth of silence) */
static void silence_page(int page) {
    memset(pcm + page * FRAMES_PER_PAGE * 2, 0, 4096);
}

void ac97_stop(void) {
    if (!present) return;
    /* The engine keeps running — playing silence IS the stopped state */
    for (int p = 0; p < RING_PAGES; p++)
        silence_page(p);
}

/*
 * PIT tick (1 kHz): keep the ring alive. Each page is ~21 ms of audio,
 * so a 1 ms cadence has plenty of margin.
 */
void ac97_tick(void) {
    if (!present) return;

    uint8_t civ = inb(nabm + NABM_PO_CIV);

    /* Re-silence the pages the hardware finished since last tick, so a
     * tone's samples never come around again on the next ring lap */
    while (prev_civ != civ) {
        silence_page(prev_civ);
        prev_civ = (uint8_t)((prev_civ + 1) % RING_PAGES);
    }

    /* Keep LVI one buffer behind CIV: the engine never reaches it, so
     * it never halts */
    outb(nabm + NABM_PO_LVI, (uint8_t)((civ + RING_PAGES - 1) % RING_PAGES));
}

void ac97_tone(uint32_t freq_hz, uint32_t ms) {
    if (!present) return;
    if (freq_hz < 20 || freq_hz > 20000) {
        ac97_stop();
        return;
    }

    /* Replace whatever is queued: silence the ring, then write the new
     * tone starting one page ahead of the hardware position (~0-21 ms
     * latency). Interrupts off so the tick can't advance CIV mid-write;
     * IF is restored to whatever the caller had (syscall handlers run
     * with interrupts already disabled). */
    uint64_t rflags;
    asm volatile("pushfq\n\tpop %0" : "=r"(rflags));
    cli();

    for (int p = 0; p < RING_PAGES; p++)
        silence_page(p);

    uint8_t civ = inb(nabm + NABM_PO_CIV);
    uint32_t start = ((uint32_t)((civ + 1) % RING_PAGES)) * FRAMES_PER_PAGE;

    uint32_t frames = (SAMPLE_RATE / 1000) * ms;
    uint32_t budget = (RING_PAGES - 2) * FRAMES_PER_PAGE;  /* Margin: 2 pages */
    if (frames > budget) frames = budget;

    uint32_t half_period = SAMPLE_RATE / (freq_hz * 2);
    if (half_period == 0) half_period = 1;

    for (uint32_t f = 0; f < frames; f++) {
        int16_t s = ((f / half_period) & 1) ? -AMPLITUDE : AMPLITUDE;
        uint32_t pos = (start + f) % RING_FRAMES;
        pcm[pos * 2]     = s;   /* Left */
        pcm[pos * 2 + 1] = s;   /* Right */
    }

    if (rflags & 0x200) sti();
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

    /* One page for the BDL, RING_PAGES contiguous pages of PCM silence */
    uint64_t bdl_page = pmm_alloc_page();
    pcm_phys = pmm_alloc_pages(RING_PAGES);
    if (bdl_page == 0 || pcm_phys == 0) return 0;
    bdl = (ac97_bdl_entry_t*)(uintptr_t)bdl_page;
    pcm = (int16_t*)(uintptr_t)pcm_phys;
    memset(bdl, 0, 4096);
    memset(pcm, 0, RING_PAGES * 4096);

    /* Static BDL: entry i -> ring page i, full page, no flags */
    for (int i = 0; i < RING_PAGES; i++) {
        bdl[i].addr  = (uint32_t)(pcm_phys + (uint64_t)i * 4096);
        bdl[i].count = FRAMES_PER_PAGE * 2;   /* samples, not frames */
        bdl[i].flags = 0;
    }

    /* Reset the PCM OUT box once, program the ring, run forever */
    outb(nabm + NABM_PO_CR, PO_CR_RR);
    uint32_t deadline = system_ticks + 100;
    while ((inb(nabm + NABM_PO_CR) & PO_CR_RR) && system_ticks < deadline)
        ;
    outl(nabm + NABM_PO_BDBAR, (uint32_t)(uintptr_t)bdl);
    outb(nabm + NABM_PO_LVI, RING_PAGES - 1);
    outw(nabm + NABM_PO_SR, 0x1C);
    prev_civ = 0;
    outb(nabm + NABM_PO_CR, PO_CR_RPBM);

    present = 1;

    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("[AC97] Codec at NAM 0x");
    terminal_writehex(nam);
    terminal_writestring(" NABM 0x");
    terminal_writehex(nabm);
    terminal_writestring(" (48 kHz ring, always running)\n");
    return 1;
}
