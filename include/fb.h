#ifndef FB_H
#define FB_H

#include "types.h"
#include "multiboot.h"

/*
 * ArcadeOS – Linear Framebuffer Driver
 *
 * Wraps the LFB the bootloader sets up via VBE. All pixels are 32-bit
 * 0x00RRGGBB. Without a framebuffer the OS falls back to VGA text mode.
 *
 * Page flipping: on QEMU's std VGA (Bochs display interface, I/O ports
 * 0x1CE/0x1CF) the driver programs a double-height virtual surface and
 * flips between the two pages with the Y-offset register — presents are
 * tear-free and the copy always lands on the hidden page. fb_ptr()
 * points at the DISPLAYED page (so boot console / panic text is always
 * visible); fb_ptr_back() is where the next frame should be drawn.
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
uint32_t* fb_ptr(void);         /* Identity-mapped pointer to the DISPLAYED page */

/* Overlay drawing on the DISPLAYED page — used by kernel UI that draws
 * over a frozen or scrubbing game (system menu, rewind indicator).
 * The game's next present simply paints over it. */
void fb_overlay_rect(int x, int y, int w, int h, uint32_t color);
void fb_overlay_text(int x, int y, const char* s, uint32_t color, int scale);

/* ──────── Page flipping (Bochs/QEMU dispi; no-ops without it) ──────── */
int  fb_flip_available(void);   /* 1 when double buffering is active */
uint32_t* fb_ptr_back(void);    /* Hidden page (render target) */
void fb_flip(void);             /* Swap displayed/hidden pages */

#endif /* FB_H */
