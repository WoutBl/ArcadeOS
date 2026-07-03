#ifndef USB_H
#define USB_H

#include "types.h"
#include "pci.h"

/*
 * ArcadeOS – USB Host Stack
 *
 * Architecture:
 *
 *   usb.c     – controller/device registry + HID report parsing
 *               (DualShock 4 reports are decoded here and fed into
 *               the gamepad subsystem)
 *   uhci.c    – UHCI (USB 1.1) host controller driver with a working
 *               transfer engine: synchronous control transfers for
 *               enumeration and a polled interrupt IN pipe for HID
 *               input reports. QEMU's PIIX3 exposes UHCI with -usb,
 *               so this is the primary path (and full-speed devices
 *               like the DualShock 4 attach to it via usb-host
 *               passthrough).
 *   xhci.c    – xHCI (USB 3.x) detection + architectural stub, to be
 *               filled in for real hardware.
 */

typedef enum {
    USB_HC_UHCI,
    USB_HC_OHCI,
    USB_HC_EHCI,
    USB_HC_XHCI,
} usb_hc_type_t;

/* An enumerated USB device with an interrupt IN pipe (HID) */
typedef struct {
    int      in_use;
    uint8_t  addr;           /* Assigned USB device address */
    int      port;           /* Root port index */
    int      low_speed;      /* 1 = low speed (1.5 Mbps) */
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t  dev_class;      /* bInterfaceClass of interface 0 */
    uint8_t  ep_in;          /* Interrupt IN endpoint number */
    uint16_t ep_in_maxpkt;   /* wMaxPacketSize of that endpoint */
    uint8_t  ep0_maxpkt;     /* bMaxPacketSize0 */
} usb_device_t;

typedef struct usb_controller {
    usb_hc_type_t  type;
    pci_device_t*  pci;
    uint32_t       io_base;     /* I/O port base (UHCI) or MMIO base (xHCI) */
    int            num_ports;
    int            ports_connected;   /* Bitmask of ports with a device */
    usb_device_t   devices[2];  /* One per root port */
    /* Driver hooks */
    void (*poll)(struct usb_controller* hc);   /* Called from the input poll loop */
} usb_controller_t;

#define USB_MAX_CONTROLLERS 4

/* ──────── Standard USB protocol constants ──────── */

/* Standard request codes */
#define USB_REQ_GET_DESCRIPTOR    6
#define USB_REQ_SET_ADDRESS       5
#define USB_REQ_SET_CONFIGURATION 9

/* Descriptor types */
#define USB_DESC_DEVICE        1
#define USB_DESC_CONFIG        2
#define USB_DESC_INTERFACE     4
#define USB_DESC_ENDPOINT      5

/* Class codes */
#define USB_CLASS_HID          3

/* Setup packet (8 bytes, sent in the SETUP stage) */
typedef struct {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed)) usb_setup_t;

/* ──────── Public API ──────── */

/* Scan PCI for USB host controllers and initialize the supported ones */
void usb_init(void);

/* Poll all registered controllers (port changes, HID reports) */
void usb_poll(void);

int               usb_controller_count(void);
usb_controller_t* usb_get_controller(int index);

/*
 * Called by HC drivers when an interrupt IN report arrives from an
 * enumerated device. usb.c decodes known layouts (DualShock 4) and
 * feeds the gamepad subsystem.
 */
void usb_hid_input(usb_device_t* dev, const uint8_t* data, int len);

/* Called by HC drivers after successful enumeration (for logging) */
void usb_announce_device(usb_device_t* dev);

/* Controller driver entry points */
int  uhci_init(usb_controller_t* hc);   /* Returns 1 on success */
void uhci_poll(usb_controller_t* hc);
int  xhci_init(usb_controller_t* hc);   /* Architectural stub */
void xhci_poll(usb_controller_t* hc);

#endif /* USB_H */
