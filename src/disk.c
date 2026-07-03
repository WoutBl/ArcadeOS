/*
 * ArcadeOS – block device dispatch (AHCI > ATA PIO)
 */

#include "disk.h"
#include "ahci.h"
#include "ata.h"
#include "vga.h"

static int use_ahci = 0;
static int present  = 0;

int disk_is_present(void) { return present; }

int disk_init(void) {
    if (ahci_init()) {
        use_ahci = 1;
        present  = 1;
        return 1;
    }

    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
    terminal_writestring("[DISK] No AHCI controller - trying legacy ATA PIO\n");

    if (ata_init()) {
        use_ahci = 0;
        present  = 1;
        return 1;
    }
    return 0;
}

int disk_read_sector(uint32_t lba, void* buffer) {
    if (!present) return 0;
    return use_ahci ? ahci_read_sector(lba, buffer)
                    : ata_read_sector(lba, buffer);
}

int disk_write_sector(uint32_t lba, const void* buffer) {
    if (!present) return 0;
    return use_ahci ? ahci_write_sector(lba, buffer)
                    : ata_write_sector(lba, buffer);
}

void disk_flush(void) {
    if (!present) return;
    if (use_ahci) ahci_flush();
    else          ata_flush();
}
