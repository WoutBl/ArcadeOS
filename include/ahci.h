#ifndef AHCI_H
#define AHCI_H

#include "types.h"

/*
 * ArcadeOS – AHCI (SATA) driver
 *
 * Minimal polled driver for one SATA disk behind an AHCI HBA: finds the
 * controller on PCI (class 0x01, subclass 0x06), initializes the first
 * port with a device attached, and issues READ/WRITE DMA EXT commands
 * through a single command slot. This is what real hardware exposes –
 * the legacy ATA PIO driver only works on IDE-emulated disks.
 */

/* Probe PCI for an AHCI HBA with a SATA disk. Returns 1 if found. */
int ahci_init(void);

int ahci_is_present(void);

/* Single-sector transfers (512 bytes), polled. Return 1 on success. */
int ahci_read_sector(uint32_t lba, void* buffer);
int ahci_write_sector(uint32_t lba, const void* buffer);

/* Issue FLUSH CACHE EXT and wait for completion */
void ahci_flush(void);

#endif /* AHCI_H */
