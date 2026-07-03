#ifndef DISK_H
#define DISK_H

#include "types.h"

/*
 * ArcadeOS – block device abstraction
 *
 * One boot disk, two possible drivers: AHCI (SATA – what real hardware
 * exposes) preferred, legacy ATA PIO (IDE emulation) as the fallback.
 * fat32.c reads and writes through this layer only.
 */

#define DISK_SECTOR_SIZE 512

/* Probe AHCI first, then ATA PIO. Returns 1 if any disk was found. */
int disk_init(void);

int disk_is_present(void);

int disk_read_sector(uint32_t lba, void* buffer);
int disk_write_sector(uint32_t lba, const void* buffer);

/* Flush the drive's write cache */
void disk_flush(void);

#endif /* DISK_H */
