#ifndef KLOG_H
#define KLOG_H

#include "types.h"

/*
 * ArcadeOS – persistent kernel log
 *
 * Every character that reaches the serial mirror is also appended to an
 * in-memory ring buffer, which the idle task periodically flushes to
 * KERNEL.LOG on the game volume. Real hardware has no serial.log file
 * to inspect after the fact — this is the on-disk equivalent. Serial
 * output itself is unaffected (logs always go to BOTH).
 */

/* Append one character (called from serial_putc; '\r' is dropped). */
void klog_putc(char c);

/* Throttled flush to KERNEL.LOG — call from the idle task. No-op unless
 * the buffer is dirty, the volume is mounted, no FAT32 operation is in
 * flight, and at least KLOG_FLUSH_MS elapsed since the last flush. */
void klog_idle_flush(void);

#endif /* KLOG_H */
