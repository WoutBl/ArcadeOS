/*
 * ArcadeOS – Linear Framebuffer Driver
 */

#include "fb.h"
#include "serial.h"

static int      fb_present = 0;
static uint32_t fb_addr    = 0;
static uint32_t fb_w       = 0;
static uint32_t fb_h       = 0;
static uint32_t fb_p       = 0;
static uint8_t  fb_depth   = 0;

int fb_init(multiboot_info_t* mboot_info) {
    fb_present = 0;

    if (!mboot_info) return 0;
    if (!(mboot_info->flags & MULTIBOOT_FLAG_FB)) return 0;
    if (mboot_info->framebuffer_type != MULTIBOOT_FB_TYPE_RGB) return 0;
    if (mboot_info->framebuffer_addr_high != 0) return 0;   /* Above 4 GiB: unreachable in 32-bit */
    if (mboot_info->framebuffer_bpp != 32) return 0;        /* Only 32-bpp supported */

    fb_addr  = mboot_info->framebuffer_addr_low;
    fb_w     = mboot_info->framebuffer_width;
    fb_h     = mboot_info->framebuffer_height;
    fb_p     = mboot_info->framebuffer_pitch;
    fb_depth = mboot_info->framebuffer_bpp;

    fb_present = 1;
    return 1;
}

int      fb_available(void)  { return fb_present; }
uint32_t fb_width(void)      { return fb_w; }
uint32_t fb_height(void)     { return fb_h; }
uint32_t fb_pitch(void)      { return fb_p; }
uint8_t  fb_bpp(void)        { return fb_depth; }
uint32_t fb_phys_addr(void)  { return fb_addr; }
uint32_t* fb_ptr(void)       { return (uint32_t*)fb_addr; }

uint32_t fb_size_bytes(void) {
    uint32_t size = fb_p * fb_h;
    /* Round up to a whole page for the paging code */
    return (size + 4095) & ~4095u;
}
