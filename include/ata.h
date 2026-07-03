#ifndef ATA_H
#define ATA_H

#include "types.h"

/* ATA Primary I/O Ports */
#define ATA_PRIMARY_IO      0x1F0
#define ATA_PRIMARY_DCR     0x3F6
#define ATA_DATA            0x1F0
#define ATA_ERROR           0x1F1
#define ATA_SECTOR_COUNT    0x1F2
#define ATA_LBA_LOW         0x1F3
#define ATA_LBA_MID         0x1F4
#define ATA_LBA_HIGH        0x1F5
#define ATA_DRIVE_SELECT    0x1F6
#define ATA_COMMAND         0x1F7
#define ATA_STATUS          0x1F7

/* ATA Commands */
#define ATA_CMD_READ_SECTORS  0x20
#define ATA_CMD_WRITE_SECTORS 0x30
#define ATA_CMD_FLUSH_CACHE   0xE7
#define ATA_CMD_IDENTIFY      0xEC

/* ATA Status Bits */
#define ATA_STATUS_BSY 0x80
#define ATA_STATUS_DRQ 0x08
#define ATA_STATUS_ERR 0x01

/* Disk layout */
#define DISK_SECTOR_SIZE 512

/* Public API */
int  ata_init(void);       /* Detect and initialize the disk; returns 1 if found */
int  ata_is_present(void);
int  ata_read_sector(uint32_t lba, void* buffer);
int  ata_write_sector(uint32_t lba, const void* buffer);
void ata_flush(void);

#endif /* ATA_H */
