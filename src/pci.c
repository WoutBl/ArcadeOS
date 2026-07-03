/*
 * ArcadeOS – PCI Bus Enumeration
 *
 * Uses configuration mechanism #1 (ports 0xCF8/0xCFC) to walk bus 0-7
 * and record every present function. The USB stack uses this table to
 * locate host controllers.
 */

#include "pci.h"
#include "vga.h"

static pci_device_t pci_devices[PCI_MAX_DEVICES];
static int          pci_num_devices = 0;

/* 32-bit port I/O helpers (types.h only provides 8/16-bit) */
static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    asm volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outl(uint16_t port, uint32_t val) {
    asm volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

uint32_t pci_read_config(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t address = 0x80000000u
                     | ((uint32_t)bus  << 16)
                     | ((uint32_t)dev  << 11)
                     | ((uint32_t)func << 8)
                     | (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

void pci_write_config(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t value) {
    uint32_t address = 0x80000000u
                     | ((uint32_t)bus  << 16)
                     | ((uint32_t)dev  << 11)
                     | ((uint32_t)func << 8)
                     | (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, value);
}

static void pci_record_function(uint8_t bus, uint8_t dev, uint8_t func) {
    if (pci_num_devices >= PCI_MAX_DEVICES) return;

    uint32_t id_reg = pci_read_config(bus, dev, func, 0x00);
    uint16_t vendor = (uint16_t)(id_reg & 0xFFFF);
    if (vendor == 0xFFFF) return;   /* No device */

    pci_device_t* d = &pci_devices[pci_num_devices++];
    d->bus       = bus;
    d->device    = dev;
    d->function  = func;
    d->vendor_id = vendor;
    d->device_id = (uint16_t)(id_reg >> 16);

    uint32_t class_reg = pci_read_config(bus, dev, func, 0x08);
    d->class_code = (uint8_t)(class_reg >> 24);
    d->subclass   = (uint8_t)(class_reg >> 16);
    d->prog_if    = (uint8_t)(class_reg >> 8);

    for (int i = 0; i < 6; i++)
        d->bar[i] = pci_read_config(bus, dev, func, (uint8_t)(0x10 + i * 4));

    d->irq_line = (uint8_t)(pci_read_config(bus, dev, func, 0x3C) & 0xFF);
}

void pci_init(void) {
    pci_num_devices = 0;

    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
    terminal_writestring("[PCI] Scanning buses...\n");

    for (uint8_t bus = 0; bus < 8; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            uint32_t id_reg = pci_read_config(bus, dev, 0, 0x00);
            if ((id_reg & 0xFFFF) == 0xFFFF) continue;

            /* Multi-function bit in the header type register */
            uint8_t header_type = (uint8_t)(pci_read_config(bus, dev, 0, 0x0C) >> 16);
            uint8_t max_func = (header_type & 0x80) ? 8 : 1;

            for (uint8_t func = 0; func < max_func; func++)
                pci_record_function(bus, dev, func);
        }
    }

    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("[PCI] Found ");
    terminal_writedec((uint32_t)pci_num_devices);
    terminal_writestring(" device functions\n");
}

pci_device_t* pci_find_class(uint8_t class_code, uint8_t subclass, int index) {
    int match = 0;
    for (int i = 0; i < pci_num_devices; i++) {
        if (pci_devices[i].class_code == class_code &&
            pci_devices[i].subclass   == subclass) {
            if (match == index) return &pci_devices[i];
            match++;
        }
    }
    return (pci_device_t*)0;
}

void pci_enable_device(pci_device_t* dev) {
    if (!dev) return;
    uint32_t cmd = pci_read_config(dev->bus, dev->device, dev->function, 0x04);
    cmd |= 0x07;   /* I/O space | memory space | bus master */
    pci_write_config(dev->bus, dev->device, dev->function, 0x04, cmd);
}

int pci_device_count(void) { return pci_num_devices; }

pci_device_t* pci_get_device(int index) {
    if (index < 0 || index >= pci_num_devices) return (pci_device_t*)0;
    return &pci_devices[index];
}
