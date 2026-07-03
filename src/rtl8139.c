/*
 * ArcadeOS – Realtek RTL8139 NIC driver (polled)
 *
 * The classic QEMU NIC (`-device rtl8139`). Everything is polled from
 * the idle task — no IRQ wiring, no locking headaches with the game
 * loop. RX uses the chip's single wrap-mode ring buffer; TX rotates
 * through the four hardware descriptors.
 */

#include "net.h"
#include "pci.h"
#include "pmm.h"
#include "vga.h"
#include "clock.h"

/* Register offsets (I/O space, BAR0) */
#define REG_IDR0      0x00    /* MAC address, 6 bytes */
#define REG_TSD0      0x10    /* TX status, 4 descriptors (dword each) */
#define REG_TSAD0     0x20    /* TX buffer address, 4 descriptors */
#define REG_RBSTART   0x30    /* RX ring physical base */
#define REG_CR        0x37    /* Command */
#define REG_CAPR      0x38    /* Current address of packet read */
#define REG_ISR       0x3E    /* Interrupt status (ack by writing 1s) */
#define REG_TCR       0x40
#define REG_RCR       0x44
#define REG_CONFIG1   0x52

#define CR_RESET      0x10
#define CR_RX_ENABLE  0x08
#define CR_TX_ENABLE  0x04
#define CR_RX_EMPTY   0x01    /* Buffer empty flag */

#define TSD_OWN       (1u << 13)   /* DMA complete (chip sets it) */

/* RX ring: 8K + 16-byte header slack + 1500 wrap overflow (WRAP mode) */
#define RX_RING_SIZE  8192
#define RX_ALLOC      (RX_RING_SIZE + 16 + 2048)

static uint16_t iobase = 0;
static int      present = 0;
static uint8_t  mac[6];

static uint8_t* rx_ring = 0;
static uint32_t rx_off  = 0;

static uint8_t* tx_buf[4];
static int      tx_next = 0;

int rtl8139_present(void) { return present; }
const uint8_t* rtl8139_mac(void) { return mac; }

int rtl8139_init(void) {
    /* Realtek 10EC:8139 — network class 0x02, subclass 0x00 */
    pci_device_t* dev = pci_find_class(0x02, 0x00, 0);
    if (!dev) return 0;

    pci_enable_device(dev);              /* I/O + bus mastering */
    iobase = (uint16_t)(dev->bar[0] & ~0x3u);
    if (!iobase) return 0;

    /* Power on, then soft reset */
    outb(iobase + REG_CONFIG1, 0x00);
    outb(iobase + REG_CR, CR_RESET);
    uint32_t deadline = system_ticks + 100;
    while ((inb(iobase + REG_CR) & CR_RESET) && system_ticks < deadline)
        ;

    for (int i = 0; i < 6; i++)
        mac[i] = inb(iobase + REG_IDR0 + i);

    /* RX ring (3 contiguous pages) + 4 TX buffers (2 KiB each = 2 pages) */
    uint64_t rx_phys = pmm_alloc_pages(RX_ALLOC / 4096 + 1);
    uint64_t tx_phys = pmm_alloc_pages(2);
    if (!rx_phys || !tx_phys) return 0;
    rx_ring = (uint8_t*)(uintptr_t)rx_phys;
    memset(rx_ring, 0, RX_ALLOC);
    for (int i = 0; i < 4; i++)
        tx_buf[i] = (uint8_t*)(uintptr_t)(tx_phys + (uint64_t)i * 2048);

    outl(iobase + REG_RBSTART, (uint32_t)rx_phys);

    /* RCR: accept phys-match + broadcast, WRAP mode, default DMA burst */
    outl(iobase + REG_RCR, (1u << 7) | 0x0A | (6u << 8));

    /* Polled: no interrupts wanted, ack anything pending */
    outw(iobase + REG_ISR, 0xFFFF);

    outb(iobase + REG_CR, CR_RX_ENABLE | CR_TX_ENABLE);
    rx_off = 0;

    present = 1;

    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("[NET] RTL8139 at I/O 0x");
    terminal_writehex(iobase);
    terminal_writestring(" MAC ");
    for (int i = 0; i < 6; i++) {
        char hex[3];
        const char* d = "0123456789ABCDEF";
        hex[0] = d[mac[i] >> 4]; hex[1] = d[mac[i] & 0xF]; hex[2] = '\0';
        terminal_writestring(hex);
        if (i < 5) terminal_writestring(":");
    }
    terminal_writestring("\n");
    return 1;
}

void rtl8139_send(const void* frame, uint32_t len) {
    if (!present || len > 1792) return;

    int d = tx_next;
    tx_next = (tx_next + 1) & 3;

    /* Wait until the chip is done with this descriptor (fast on QEMU) */
    uint32_t deadline = system_ticks + 50;
    uint32_t tsd = inl(iobase + REG_TSD0 + d * 4);
    while (!(tsd & TSD_OWN) && tsd != 0 && system_ticks < deadline)
        tsd = inl(iobase + REG_TSD0 + d * 4);

    memcpy(tx_buf[d], frame, len);
    if (len < 60) {                       /* Ethernet minimum frame */
        memset(tx_buf[d] + len, 0, 60 - len);
        len = 60;
    }
    outl(iobase + REG_TSAD0 + d * 4, (uint32_t)(uintptr_t)tx_buf[d]);
    outl(iobase + REG_TSD0  + d * 4, len);   /* Clears OWN: chip sends */
}

uint32_t rtl8139_recv(void* buf, uint32_t maxlen) {
    if (!present) return 0;
    if (inb(iobase + REG_CR) & CR_RX_EMPTY) return 0;

    /* Packet header: u16 status, u16 length (includes 4-byte CRC) */
    uint16_t status = *(uint16_t*)(rx_ring + rx_off);
    uint16_t length = *(uint16_t*)(rx_ring + rx_off + 2);

    uint32_t copied = 0;
    if ((status & 0x01) && length >= 4) {     /* ROK */
        uint32_t payload = length - 4;        /* Strip CRC */
        if (payload > maxlen) payload = maxlen;
        memcpy(buf, rx_ring + rx_off + 4, payload);
        copied = payload;
    }

    /* Advance to the next packet, dword aligned */
    rx_off = (rx_off + length + 4 + 3) & ~3u;
    if (rx_off >= RX_RING_SIZE) rx_off -= RX_RING_SIZE;
    outw(iobase + REG_CAPR, (uint16_t)(rx_off - 0x10));

    /* Ack RX status bits */
    outw(iobase + REG_ISR, 0x0001);
    return copied;
}
