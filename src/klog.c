/*
 * ArcadeOS – persistent kernel log (see klog.h)
 */

#include "klog.h"
#include "fat32.h"
#include "clock.h"

#define KLOG_CAP      32768          /* Ring capacity (fat32_save whole-file) */
#define KLOG_FLUSH_MS 2000           /* Min interval between disk flushes */

static char     klog_buf[KLOG_CAP];
static uint32_t klog_len   = 0;
static int      klog_dirty = 0;
static int      flushing   = 0;
static uint32_t last_flush = 0;

void klog_putc(char c) {
    if (c == '\r') return;

    if (klog_len >= KLOG_CAP) {
        if (flushing) return;        /* Never shift the buffer mid-write */
        /* Drop the oldest half so recent history always survives */
        memcpy(klog_buf, klog_buf + KLOG_CAP / 2, KLOG_CAP / 2);
        klog_len = KLOG_CAP / 2;
    }
    klog_buf[klog_len++] = c;
    klog_dirty = 1;
}

void klog_idle_flush(void) {
    if (!klog_dirty) return;
    if (!fat32_available()) return;
    if (system_ticks - last_flush < KLOG_FLUSH_MS) return;

    /*
     * Interrupts off for the whole flush: on a single CPU this
     * guarantees no game save/load interleaves with our volume write.
     * If a preempted FAT32 op is mid-flight, skip and retry next round.
     */
    cli();
    if (fat32_busy()) {
        sti();
        return;
    }
    flushing = 1;
    fat32_save("KERNEL.LOG", (const uint8_t*)klog_buf, klog_len);
    flushing = 0;
    klog_dirty = 0;
    last_flush = system_ticks;
    sti();
}
