#ifndef PCI_H
#define PCI_H

#include "types.h"

/*
 * ArcadeOS – PCI Configuration Space Access (mechanism #1)
 */

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

/* Common class codes */
#define PCI_CLASS_SERIAL_BUS 0x0C
#define PCI_SUBCLASS_USB     0x03

/* USB programming interfaces (prog_if for class 0x0C:0x03) */
#define PCI_USB_PROGIF_UHCI  0x00
#define PCI_USB_PROGIF_OHCI  0x10
#define PCI_USB_PROGIF_EHCI  0x20
#define PCI_USB_PROGIF_XHCI  0x30

typedef struct {
    uint8_t  bus;
    uint8_t  device;
    uint8_t  function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint32_t bar[6];
    uint8_t  irq_line;
} pci_device_t;

#define PCI_MAX_DEVICES 32

/* Scan all buses and populate the device table */
void pci_init(void);

/* Raw config-space access */
uint32_t pci_read_config(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
void     pci_write_config(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t value);

/* Find the Nth device matching class/subclass (returns NULL when exhausted) */
pci_device_t* pci_find_class(uint8_t class_code, uint8_t subclass, int index);

/* Enable I/O + memory decoding + bus mastering for a device */
void pci_enable_device(pci_device_t* dev);

int           pci_device_count(void);
pci_device_t* pci_get_device(int index);

#endif /* PCI_H */
