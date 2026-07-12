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

/* Copy the current ring contents into out (no NUL). Returns bytes
 * copied. Used by the REST API's /api/log endpoint. */
int klog_read(char* out, int maxlen);

/* Unthrottled flush for panic paths. The panic message has already been
 * mirrored into the ring; write it to KERNEL.LOG NOW, because the halt
 * loop that follows never returns to the idle task. Skipped only when a
 * FAT32 operation was interrupted mid-flight — reentering the driver
 * then could corrupt the volume. */
void klog_panic_flush(void);

#endif /* KLOG_H */
