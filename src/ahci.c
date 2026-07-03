/*
 * ArcadeOS – AHCI (SATA) driver
 *
 * Polled, single-slot, single-port. The HBA's ABAR (BAR5) is MMIO and
 * lives above the RAM identity map, so it is mapped cache-disabled via
 * paging_kernel_map_mmio() before first touch.
 *
 * DMA structures (command list, received FIS, command table) share one
 * 4 KiB page from the PMM: the page is 4 KiB-aligned, which satisfies
 * the 1 KiB (command list), 256 B (FIS) and 128 B (command table)
 * alignment rules. All physical addresses fit in 32 bits (RAM < 4 GiB).
 */

#include "ahci.h"
#include "pci.h"
#include "paging.h"
#include "pmm.h"
#include "vga.h"
#include "clock.h"

/* ──────── HBA register layout ──────── */

typedef volatile struct {
    uint32_t cap;       /* 0x00 Host capabilities */
    uint32_t ghc;       /* 0x04 Global host control */
    uint32_t is;        /* 0x08 Interrupt status */
    uint32_t pi;        /* 0x0C Ports implemented (bitmap) */
    uint32_t vs;        /* 0x10 Version */
} hba_host_t;

typedef volatile struct {
    uint32_t clb;       /* 0x00 Command list base (1 KiB aligned) */
    uint32_t clbu;      /* 0x04 ... upper 32 bits */
    uint32_t fb;        /* 0x08 FIS receive base (256 B aligned) */
    uint32_t fbu;       /* 0x0C ... upper 32 bits */
    uint32_t is;        /* 0x10 Interrupt status */
    uint32_t ie;        /* 0x14 Interrupt enable */
    uint32_t cmd;       /* 0x18 Command and status */
    uint32_t rsv0;
    uint32_t tfd;       /* 0x20 Task file data */
    uint32_t sig;       /* 0x24 Signature */
    uint32_t ssts;      /* 0x28 SATA status (SStatus) */
    uint32_t sctl;      /* 0x2C SATA control */
    uint32_t serr;      /* 0x30 SATA error */
    uint32_t sact;      /* 0x34 SATA active */
    uint32_t ci;        /* 0x38 Command issue */
} hba_port_t;

#define HBA_PORT_BASE(abar)   ((uintptr_t)(abar) + 0x100)
#define HBA_PORT(abar, n)     ((hba_port_t*)(HBA_PORT_BASE(abar) + (n) * 0x80))

#define GHC_AE          (1u << 31)   /* AHCI enable */

#define PORT_CMD_ST     (1u << 0)    /* Start command engine */
#define PORT_CMD_FRE    (1u << 4)    /* FIS receive enable */
#define PORT_CMD_FR     (1u << 14)   /* FIS receive running */
#define PORT_CMD_CR     (1u << 15)   /* Command list running */

#define PORT_IS_TFES    (1u << 30)   /* Task file error */

#define SSTS_DET_PRESENT 3           /* Device present + phy established */
#define SATA_SIG_DISK    0x00000101

#define TFD_BSY         0x80
#define TFD_DRQ         0x08
#define TFD_ERR         0x01

#define ATA_CMD_READ_DMA_EXT  0x25
#define ATA_CMD_WRITE_DMA_EXT 0x35

/* ──────── Command structures (in the DMA page) ──────── */

typedef struct {
    uint16_t flags;      /* CFL (dwords), W bit, PRDTL in the high word */
    uint16_t prdtl;      /* PRDT entry count */
    volatile uint32_t prdbc; /* Bytes transferred (written back by HBA) */
    uint32_t ctba;       /* Command table base */
    uint32_t ctbau;
    uint32_t rsv[4];
} __attribute__((packed)) hba_cmd_header_t;

typedef struct {
    uint8_t  cfis[64];   /* Command FIS */
    uint8_t  acmd[16];   /* ATAPI command (unused) */
    uint8_t  rsv[48];
    /* PRDT entry 0 */
    uint32_t dba;        /* Data base address */
    uint32_t dbau;
    uint32_t rsv2;
    uint32_t dbc;        /* Byte count - 1 (bit 31 = IOC, unused: polling) */
} __attribute__((packed)) hba_cmd_table_t;

/* ──────── Driver state ──────── */

static hba_host_t* hba  = 0;
static hba_port_t* port = 0;
static hba_cmd_header_t* cmd_list = 0;   /* Slot 0 only */
static hba_cmd_table_t*  cmd_table = 0;
static int ahci_present = 0;

int ahci_is_present(void) { return ahci_present; }

/* ──────── Port bring-up ──────── */

static void port_stop(void) {
    port->cmd &= ~PORT_CMD_ST;
    port->cmd &= ~PORT_CMD_FRE;
    uint32_t deadline = system_ticks + 500;
    while ((port->cmd & (PORT_CMD_CR | PORT_CMD_FR)) && system_ticks < deadline)
        ;
}

static void port_start(void) {
    while (port->cmd & PORT_CMD_CR)
        ;
    port->cmd |= PORT_CMD_FRE;
    port->cmd |= PORT_CMD_ST;
}

/* ──────── Command execution (slot 0, polled) ──────── */

static int ahci_rw(uint32_t lba, void* buffer, int write) {
    if (!ahci_present) return 0;

    /* Header: 5-dword CFIS, 1 PRDT entry, direction */
    cmd_list->flags = 5 | (write ? (1 << 6) : 0);
    cmd_list->prdtl = 1;
    cmd_list->prdbc = 0;

    /* PRDT: one 512-byte buffer */
    cmd_table->dba  = (uint32_t)(uintptr_t)buffer;
    cmd_table->dbau = 0;
    cmd_table->dbc  = 512 - 1;

    /* Register H2D FIS: READ/WRITE DMA EXT, 48-bit LBA, 1 sector */
    uint8_t* fis = cmd_table->cfis;
    memset(cmd_table->cfis, 0, 64);
    fis[0] = 0x27;                       /* FIS type: register H2D */
    fis[1] = 0x80;                       /* C bit: command */
    fis[2] = write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT;
    fis[4] = (uint8_t)lba;
    fis[5] = (uint8_t)(lba >> 8);
    fis[6] = (uint8_t)(lba >> 16);
    fis[7] = 0x40;                       /* Device: LBA mode */
    fis[8] = (uint8_t)(lba >> 24);
    fis[12] = 1;                         /* Sector count = 1 */

    /* Wait for the port to go idle, then issue slot 0 */
    uint32_t deadline = system_ticks + 1000;
    while ((port->tfd & (TFD_BSY | TFD_DRQ)) && system_ticks < deadline)
        ;
    if (port->tfd & (TFD_BSY | TFD_DRQ)) return 0;

    port->is = 0xFFFFFFFF;               /* Clear stale status */
    port->ci = 1;

    while ((port->ci & 1) && system_ticks < deadline) {
        if (port->is & PORT_IS_TFES) return 0;
    }
    if (port->ci & 1) return 0;          /* Timeout */
    if ((port->is & PORT_IS_TFES) || (port->tfd & TFD_ERR)) return 0;

    return 1;
}

int ahci_read_sector(uint32_t lba, void* buffer) {
    return ahci_rw(lba, buffer, 0);
}

int ahci_write_sector(uint32_t lba, const void* buffer) {
    return ahci_rw(lba, (void*)buffer, 1);
}

void ahci_flush(void) {
    if (!ahci_present) return;

    cmd_list->flags = 5;                 /* 5-dword CFIS, no data */
    cmd_list->prdtl = 0;
    cmd_list->prdbc = 0;

    memset(cmd_table->cfis, 0, 64);
    uint8_t* fis = cmd_table->cfis;
    fis[0] = 0x27;                       /* Register H2D */
    fis[1] = 0x80;                       /* C bit */
    fis[2] = 0xEA;                       /* FLUSH CACHE EXT */
    fis[7] = 0x40;

    uint32_t deadline = system_ticks + 1000;
    while ((port->tfd & (TFD_BSY | TFD_DRQ)) && system_ticks < deadline)
        ;
    port->is = 0xFFFFFFFF;
    port->ci = 1;
    while ((port->ci & 1) && system_ticks < deadline)
        ;
}

/* ──────── Initialization ──────── */

int ahci_init(void) {
    pci_device_t* dev = pci_find_class(0x01, 0x06, 0);  /* Mass storage / SATA */
    if (!dev) return 0;

    pci_enable_device(dev);

    uint64_t abar = dev->bar[5] & ~(uint64_t)0xFFF;
    if (abar == 0) return 0;

    /* The ABAR is MMIO above RAM – map it before the first register read */
    paging_kernel_map_mmio(abar, 0x1100 + 32 * 0x80);

    hba = (hba_host_t*)(uintptr_t)abar;
    hba->ghc |= GHC_AE;

    /* Find the first implemented port with a SATA disk attached */
    uint32_t pi = hba->pi;
    int found_port = -1;
    for (int p = 0; p < 32; p++) {
        if (!(pi & (1u << p))) continue;
        hba_port_t* pt = HBA_PORT(abar, p);
        if ((pt->ssts & 0x0F) != SSTS_DET_PRESENT) continue;
        if (pt->sig != SATA_SIG_DISK) continue;
        found_port = p;
        break;
    }
    if (found_port < 0) return 0;

    port = HBA_PORT(abar, found_port);

    /* One DMA page: command list @0, received FIS @0x400, table @0x500 */
    uint64_t dma_page = pmm_alloc_page();
    if (dma_page == 0) return 0;
    memset((void*)(uintptr_t)dma_page, 0, 4096);

    cmd_list  = (hba_cmd_header_t*)(uintptr_t)dma_page;
    cmd_table = (hba_cmd_table_t*)(uintptr_t)(dma_page + 0x500);

    port_stop();
    port->clb  = (uint32_t)dma_page;
    port->clbu = 0;
    port->fb   = (uint32_t)(dma_page + 0x400);
    port->fbu  = 0;
    port->serr = 0xFFFFFFFF;             /* Clear sticky errors */
    port->is   = 0xFFFFFFFF;
    cmd_list->ctba  = (uint32_t)(dma_page + 0x500);
    cmd_list->ctbau = 0;
    port_start();

    ahci_present = 1;

    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("[AHCI] SATA disk on port ");
    terminal_writedec((uint32_t)found_port);
    terminal_writestring(" (ABAR 0x");
    terminal_writehex((uint32_t)abar);
    terminal_writestring(")\n");

    return 1;
}
