/*
 * ArcadeOS – ATA PIO Disk Driver (with timeout safety)
 */

#include "ata.h"
#include "vga.h"
#include "clock.h"

/* Internal state */
static int disk_present = 0;

/* Maximum iterations before declaring a timeout */
#define ATA_TIMEOUT 100000

/* ──────── Timeout-safe wait helpers ──────── */
static int ata_wait_bsy(void) {
    uint32_t timeout = ATA_TIMEOUT;
    while ((inb(ATA_STATUS) & ATA_STATUS_BSY) && --timeout)
        ;
    return timeout > 0;
}

static int ata_wait_drq(void) {
    uint32_t timeout = ATA_TIMEOUT;
    while (!(inb(ATA_STATUS) & ATA_STATUS_DRQ) && --timeout)
        ;
    return timeout > 0;
}

/* Read alternate status register 4 times (~400 ns delay) */
static void ata_400ns_delay(void) {
    inb(ATA_PRIMARY_DCR);
    inb(ATA_PRIMARY_DCR);
    inb(ATA_PRIMARY_DCR);
    inb(ATA_PRIMARY_DCR);
}

/* Longer delay loop (~2ms at typical speeds) */
static void ata_soft_delay(void) {
    for (volatile uint32_t i = 0; i < 10000; i++)
        ;
}

/* ──────── Public API ──────── */
int ata_is_present(void) {
    return disk_present;
}

int ata_init(void) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
    terminal_writestring("[ATA] Probing primary IDE master...\n");

    /* Software reset the ATA bus */
    outb(ATA_PRIMARY_DCR, 0x04);   /* Set SRST bit */
    ata_soft_delay();              /* Wait > 5 µs */
    outb(ATA_PRIMARY_DCR, 0x02);   /* Clear SRST, keep nIEN=1 (disable IRQ14) */
    ata_soft_delay();              /* Wait at least 2 ms for drive to settle */
    ata_soft_delay();
    ata_soft_delay();

    /* Wait for BSY to clear after reset */
    if (!ata_wait_bsy()) {
        terminal_writestring("[ATA] Timeout waiting for BSY after reset\n");
        disk_present = 0;
        return 0;
    }

    /* Select master drive */
    outb(ATA_DRIVE_SELECT, 0xA0);
    ata_400ns_delay();

    /* Quick presence check: read status. 0xFF means floating bus (no device) */
    uint8_t status = inb(ATA_STATUS);
    if (status == 0xFF) {
        terminal_writestring("[ATA] No device on bus (floating)\n");
        disk_present = 0;
        return 0;
    }

    /* Send IDENTIFY command */
    outb(ATA_SECTOR_COUNT, 0);
    outb(ATA_LBA_LOW, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HIGH, 0);
    outb(ATA_COMMAND, ATA_CMD_IDENTIFY);
    ata_400ns_delay();

    /* Read status after IDENTIFY */
    status = inb(ATA_STATUS);
    if (status == 0) {
        terminal_writestring("[ATA] IDENTIFY returned status 0 (no disk)\n");
        disk_present = 0;
        return 0;
    }

    /* Wait for BSY to clear */
    if (!ata_wait_bsy()) {
        terminal_writestring("[ATA] Timeout during IDENTIFY\n");
        disk_present = 0;
        return 0;
    }

    /* Check that LBA_MID and LBA_HIGH are still 0 (ATAPI sets them non-zero) */
    uint8_t mid = inb(ATA_LBA_MID);
    uint8_t high = inb(ATA_LBA_HIGH);
    if (mid != 0 || high != 0) {
        terminal_writestring("[ATA] Device is ATAPI, not ATA\n");
        disk_present = 0;
        return 0;
    }

    /* Poll for DRQ or ERR (with timeout) */
    uint32_t timeout = ATA_TIMEOUT;
    while (timeout--) {
        status = inb(ATA_STATUS);
        if (status & ATA_STATUS_ERR) {
            terminal_writestring("[ATA] IDENTIFY returned error\n");
            disk_present = 0;
            return 0;
        }
        if (status & ATA_STATUS_DRQ) break;
    }
    if (timeout == 0) {
        terminal_writestring("[ATA] Timeout waiting for DRQ\n");
        disk_present = 0;
        return 0;
    }

    /* Read and discard the 256-word identify data */
    for (int i = 0; i < 256; i++)
        inw(ATA_DATA);

    /* Re-enable IRQ14 by clearing nIEN */
    outb(ATA_PRIMARY_DCR, 0x00);

    disk_present = 1;
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("[ATA] Primary master detected successfully\n");
    return 1;
}

int ata_read_sector(uint32_t lba, void* buffer) {
    if (!disk_present) return 0;
    if (!ata_wait_bsy()) return 0;

    outb(ATA_DRIVE_SELECT, 0xE0 | ((lba >> 24) & 0x0F));
    ata_400ns_delay();
    outb(ATA_SECTOR_COUNT, 1);
    outb(ATA_LBA_LOW, lba & 0xFF);
    outb(ATA_LBA_MID, (lba >> 8) & 0xFF);
    outb(ATA_LBA_HIGH, (lba >> 16) & 0xFF);
    outb(ATA_COMMAND, ATA_CMD_READ_SECTORS);
    ata_400ns_delay();

    if (!ata_wait_bsy()) return 0;
    if (!ata_wait_drq()) return 0;

    uint8_t status = inb(ATA_STATUS);
    if (status & ATA_STATUS_ERR) return 0;

    uint16_t* buf = (uint16_t*)buffer;
    for (int i = 0; i < 256; i++)
        buf[i] = inw(ATA_DATA);

    return 1;
}

int ata_write_sector(uint32_t lba, const void* buffer) {
    if (!disk_present) return 0;
    if (!ata_wait_bsy()) return 0;

    outb(ATA_DRIVE_SELECT, 0xE0 | ((lba >> 24) & 0x0F));
    ata_400ns_delay();
    outb(ATA_SECTOR_COUNT, 1);
    outb(ATA_LBA_LOW, lba & 0xFF);
    outb(ATA_LBA_MID, (lba >> 8) & 0xFF);
    outb(ATA_LBA_HIGH, (lba >> 16) & 0xFF);
    outb(ATA_COMMAND, ATA_CMD_WRITE_SECTORS);
    ata_400ns_delay();

    if (!ata_wait_bsy()) return 0;

    uint8_t status = inb(ATA_STATUS);
    if (status & ATA_STATUS_ERR) return 0;

    if (!ata_wait_drq()) return 0;

    const uint16_t* buf = (const uint16_t*)buffer;
    for (int i = 0; i < 256; i++)
        outw(ATA_DATA, buf[i]);

    ata_400ns_delay();
    if (!ata_wait_bsy()) return 0;

    status = inb(ATA_STATUS);
    if (status & ATA_STATUS_ERR) return 0;

    return 1;
}

void ata_flush(void) {
    if (!disk_present) return;
    if (!ata_wait_bsy()) return;

    outb(ATA_DRIVE_SELECT, 0xE0);
    ata_400ns_delay();
    outb(ATA_COMMAND, ATA_CMD_FLUSH_CACHE);
    ata_400ns_delay();
    ata_wait_bsy();
}
