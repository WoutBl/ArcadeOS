/*
 * ArcadeOS – COM1 Debug Serial Port
 *
 * All terminal output is mirrored here so the console can be debugged
 * headless (QEMU: -serial file:serial.log).
 */

#include "serial.h"
#include "klog.h"

static int serial_ready = 0;

void serial_init(void) {
    outb(SERIAL_COM1 + 1, 0x00);    /* Disable interrupts */
    outb(SERIAL_COM1 + 3, 0x80);    /* Enable DLAB (set baud rate divisor) */
    outb(SERIAL_COM1 + 0, 0x01);    /* Divisor 1 (lo byte) → 115200 baud */
    outb(SERIAL_COM1 + 1, 0x00);    /*           (hi byte) */
    outb(SERIAL_COM1 + 3, 0x03);    /* 8 bits, no parity, one stop bit */
    outb(SERIAL_COM1 + 2, 0xC7);    /* Enable FIFO, clear, 14-byte threshold */
    outb(SERIAL_COM1 + 4, 0x0B);    /* IRQs enabled, RTS/DSR set */
    serial_ready = 1;
}

static int serial_tx_empty(void) {
    return inb(SERIAL_COM1 + 5) & 0x20;
}

void serial_putc(char c) {
    if (!serial_ready) return;
    klog_putc(c);               /* Mirror into the on-disk kernel log */
    if (c == '\n') serial_putc('\r');

    uint32_t timeout = 100000;
    while (!serial_tx_empty() && --timeout)
        ;
    outb(SERIAL_COM1, (uint8_t)c);
}

void serial_write(const char* str) {
    while (*str)
        serial_putc(*str++);
}
