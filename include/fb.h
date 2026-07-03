#ifndef FB_H
#define FB_H

#include "types.h"
#include "multiboot.h"

/*
 * ArcadeOS – Linear Framebuffer Driver
 *
 * Wraps the LFB that GRUB sets up for us (requested in boot.asm).
 * All pixels are 32-bit 0x00RRGGBB. If GRUB could not provide a
 * framebuffer the OS falls back to VGA text mode (see vga.c).
 */

/* Capture framebuffer info from the multiboot structure.
 * Returns 1 if a usable 32-bpp RGB linear framebuffer is present. */
int fb_init(multiboot_info_t* mboot_info);

int      fb_available(void);
uint32_t fb_width(void);
uint32_t fb_height(void);
uint32_t fb_pitch(void);        /* Bytes per scanline */
uint8_t  fb_bpp(void);
uint32_t fb_phys_addr(void);    /* Physical address of the LFB */
uint32_t fb_size_bytes(void);   /* pitch * height, page-rounded */
uint32_t* fb_ptr(void);         /* Identity-mapped pointer to the LFB */

#endif /* FB_H */
