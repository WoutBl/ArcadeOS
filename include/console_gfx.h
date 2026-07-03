#ifndef CONSOLE_GFX_H
#define CONSOLE_GFX_H

#include "types.h"

/*
 * ArcadeOS – Console Graphics API
 *
 * Double-buffered 2D drawing layer on top of the linear framebuffer (fb.c).
 * All drawing calls target the back buffer; gfx_present() flips it to the
 * screen. This is the foundation a hardware-accelerated GPU driver will
 * later slot underneath (the API stays, the blitter changes).
 *
 * Colors are 32-bit 0x00RRGGBB (alpha byte ignored on the framebuffer,
 * but sprites use alpha 0x00 = transparent).
 */

/* Sprite: w*h pixels, row-major. Pixels with alpha byte 0 are skipped. */
typedef struct {
    uint16_t        w;
    uint16_t        h;
    const uint32_t* pixels;
} gfx_sprite_t;

static inline uint32_t gfx_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* Allocate the back buffer (contiguous PMM pages). Returns 1 on success.
 * Must be called after pmm_init() + paging_init(). */
int gfx_init(void);

int gfx_ready(void);

/* ──────── Back-buffer drawing primitives ──────── */
void gfx_clear(uint32_t color);
void gfx_put_pixel(int x, int y, uint32_t color);
void gfx_fill_rect(int x, int y, int w, int h, uint32_t color);
void gfx_draw_rect(int x, int y, int w, int h, uint32_t color);   /* Outline */
void gfx_draw_line(int x0, int y0, int x1, int y1, uint32_t color);
void gfx_draw_sprite(int x, int y, const gfx_sprite_t* sprite);

/* Text (8x8 font, integer scale). bg = GFX_TRANSPARENT skips background. */
#define GFX_TRANSPARENT 0xFF000000u
void gfx_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg, int scale);
void gfx_draw_text(int x, int y, const char* str, uint32_t fg, uint32_t bg, int scale);

/* ──────── Presentation ──────── */

/* Copy the back buffer to the framebuffer (the "flip"). */
void gfx_present(void);

/* Copy a full-screen pixel buffer (width*height 32-bit pixels) straight
 * to the framebuffer. Used by the SYS_GFX_PRESENT syscall so user-space
 * games can render into their own buffer and present in one call. */
void gfx_present_buffer(const uint32_t* pixels);

/* ──────── Direct front-buffer glyph rendering ────────
 * Used by the boot console (vga.c) so log text is visible without
 * having to present the whole back buffer on every character. */
void gfx_front_char(int px, int py, char c, uint32_t fg, uint32_t bg);
void gfx_front_scroll(int row_px, uint32_t bg);   /* Scroll front buffer up by row_px pixels */
void gfx_front_clear(uint32_t color);

#endif /* CONSOLE_GFX_H */
