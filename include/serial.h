#ifndef SERIAL_H
#define SERIAL_H

#include "types.h"

/* COM1 base port */
#define SERIAL_COM1 0x3F8

/*
 * Kernel debug serial port (COM1, 115200 8N1).
 * The terminal driver mirrors all console output here so the OS can be
 * debugged headless (QEMU: -serial file:serial.log).
 */
void serial_init(void);
void serial_putc(char c);
void serial_write(const char* str);

#endif /* SERIAL_H */
