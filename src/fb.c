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

/* ──────── Bochs display interface (QEMU std VGA) ──────── */
#define DISPI_INDEX     0x01CE
#define DISPI_DATA      0x01CF
#define DISPI_ID        0
#define DISPI_XRES      1
#define DISPI_YRES      2
#define DISPI_BPP       3
#define DISPI_ENABLE    4
#define DISPI_VIRT_H    7
#define DISPI_Y_OFFSET  9

static int flip_ok   = 0;    /* Dispi found: double buffering active */
static int cur_front = 0;    /* Which page is displayed (0 or 1) */

static uint16_t dispi_read(uint16_t reg) {
    outw(DISPI_INDEX, reg);
    return inw(DISPI_DATA);
}

static void dispi_write(uint16_t reg, uint16_t val) {
    outw(DISPI_INDEX, reg);
    outw(DISPI_DATA, val);
}

/* Reprogram the current mode with a double-height virtual surface so we
 * can flip between two pages via the Y-offset register. */
static void fb_try_enable_flipping(void) {
    uint16_t id = dispi_read(DISPI_ID);
    if (id < 0xB0C0 || id > 0xB0C5) return;   /* No Bochs dispi */

    dispi_write(DISPI_ENABLE, 0);
    dispi_write(DISPI_XRES, (uint16_t)fb_w);
    dispi_write(DISPI_YRES, (uint16_t)fb_h);
    dispi_write(DISPI_BPP, 32);
    dispi_write(DISPI_VIRT_H, (uint16_t)(fb_h * 2));
    dispi_write(DISPI_Y_OFFSET, 0);
    dispi_write(DISPI_ENABLE, 0x01 | 0x40);   /* Enabled + LFB */

    cur_front = 0;
    flip_ok = 1;
    serial_write("[FB] Bochs dispi: page flipping enabled (double-height VRAM)\n");
}

int fb_flip_available(void) { return flip_ok; }

uint32_t* fb_ptr_back(void) {
    if (!flip_ok) return (uint32_t*)(uintptr_t)fb_addr;
    uint32_t page = cur_front ? 0 : fb_h;     /* The hidden page */
    return (uint32_t*)(uintptr_t)(fb_addr + page * fb_p);
}

void fb_flip(void) {
    if (!flip_ok) return;
    cur_front ^= 1;
    dispi_write(DISPI_Y_OFFSET, cur_front ? (uint16_t)fb_h : 0);
}

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

    /* Upgrade to a flip-capable double-height surface when the VGA
     * exposes the Bochs display interface (QEMU/Bochs/VirtualBox). */
    fb_try_enable_flipping();
    return 1;
}

int      fb_available(void)  { return fb_present; }
uint32_t fb_width(void)      { return fb_w; }
uint32_t fb_height(void)     { return fb_h; }
uint32_t fb_pitch(void)      { return fb_p; }
uint8_t  fb_bpp(void)        { return fb_depth; }
uint32_t fb_phys_addr(void)  { return fb_addr; }
uint32_t* fb_ptr(void) {
    if (!flip_ok || cur_front == 0) return (uint32_t*)(uintptr_t)fb_addr;
    return (uint32_t*)(uintptr_t)(fb_addr + fb_h * fb_p);
}

uint32_t fb_size_bytes(void) {
    uint32_t size = fb_p * fb_h;
    /* Round up to a whole page for the paging code */
    return (size + 4095) & ~4095u;
}

/* ──────── Overlay drawing (see fb.h) ──────── */

#include "font8x8.h"

/* Overlays paint BOTH pages of a flip-capable surface: the visible one
 * for the player, and the hidden one so neither the next flip nor a
 * QEMU screendump (which reads from VRAM start) loses the overlay. */
static void overlay_rect_on(uint32_t* fb, int x, int y, int w, int h,
                            uint32_t color) {
    uint32_t pitch = fb_p / 4;
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            fb[(uint32_t)(y + r) * pitch + (uint32_t)(x + c)] = color;
}

void fb_overlay_rect(int x, int y, int w, int h, uint32_t color) {
    if (!fb_present) return;
    overlay_rect_on(fb_ptr(), x, y, w, h, color);
    if (fb_ptr_back() != fb_ptr())
        overlay_rect_on(fb_ptr_back(), x, y, w, h, color);
}

static void overlay_text_on(uint32_t* fb, int x, int y, const char* s,
                            uint32_t color, int scale) {
    uint32_t pitch = fb_p / 4;
    for (int i = 0; s[i]; i++) {
        const uint8_t* g = font8x8_basic[(uint8_t)s[i] & 0x7F];
        for (int r = 0; r < 8; r++)
            for (int b = 0; b < 8; b++) {
                if (!(g[r] & (1 << b))) continue;
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++)
                        fb[(uint32_t)(y + r * scale + sy) * pitch
                           + (uint32_t)(x + i * 8 * scale + b * scale + sx)] = color;
            }
    }
}

void fb_overlay_text(int x, int y, const char* s, uint32_t color, int scale) {
    if (!fb_present) return;
    overlay_text_on(fb_ptr(), x, y, s, color, scale);
    if (fb_ptr_back() != fb_ptr())
        overlay_text_on(fb_ptr_back(), x, y, s, color, scale);
}
