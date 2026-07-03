/*
 * ArcadeOS – Console Graphics API (double-buffered software renderer)
 */

#include "console_gfx.h"
#include "fb.h"
#include "pmm.h"
#include "vga.h"
#include "font8x8.h"

/* Back buffer: width*height 32-bit pixels, allocated from contiguous PMM pages */
static uint32_t* backbuffer  = NULL;
static int       gfx_is_ready = 0;
static int       scr_w = 0;
static int       scr_h = 0;

int gfx_ready(void) { return gfx_is_ready; }

int gfx_init(void) {
    if (!fb_available()) {
        terminal_writestring("[GFX] No framebuffer - graphics disabled\n");
        return 0;
    }

    scr_w = (int)fb_width();
    scr_h = (int)fb_height();

    uint32_t bytes = (uint32_t)scr_w * (uint32_t)scr_h * 4;
    uint32_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    uint32_t phys = pmm_alloc_pages(pages);
    if (phys == 0) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("[GFX] FATAL: Could not allocate back buffer!\n");
        return 0;
    }

    backbuffer = (uint32_t*)(uintptr_t)phys;   /* Identity-mapped RAM */
    memset(backbuffer, 0, bytes);
    gfx_is_ready = 1;

    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("[GFX] Double buffer ready: ");
    terminal_writedec((uint32_t)scr_w);
    terminal_writestring("x");
    terminal_writedec((uint32_t)scr_h);
    terminal_writestring("x32 (");
    terminal_writedec(pages);
    terminal_writestring(" pages)\n");
    return 1;
}

/* ──────── Back-buffer primitives ──────── */

void gfx_clear(uint32_t color) {
    if (!gfx_is_ready) return;
    int count = scr_w * scr_h;
    for (int i = 0; i < count; i++)
        backbuffer[i] = color;
}

void gfx_put_pixel(int x, int y, uint32_t color) {
    if (!gfx_is_ready) return;
    if (x < 0 || y < 0 || x >= scr_w || y >= scr_h) return;
    backbuffer[y * scr_w + x] = color;
}

void gfx_fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (!gfx_is_ready) return;

    /* Clip to screen */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > scr_w) w = scr_w - x;
    if (y + h > scr_h) h = scr_h - y;
    if (w <= 0 || h <= 0) return;

    for (int row = y; row < y + h; row++) {
        uint32_t* dst = &backbuffer[row * scr_w + x];
        for (int col = 0; col < w; col++)
            dst[col] = color;
    }
}

void gfx_draw_rect(int x, int y, int w, int h, uint32_t color) {
    gfx_fill_rect(x,         y,         w, 1, color);   /* Top */
    gfx_fill_rect(x,         y + h - 1, w, 1, color);   /* Bottom */
    gfx_fill_rect(x,         y,         1, h, color);   /* Left */
    gfx_fill_rect(x + w - 1, y,         1, h, color);   /* Right */
}

void gfx_draw_line(int x0, int y0, int x1, int y1, uint32_t color) {
    /* Bresenham's line algorithm */
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    while (1) {
        gfx_put_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

void gfx_draw_sprite(int x, int y, const gfx_sprite_t* sprite) {
    if (!gfx_is_ready || !sprite || !sprite->pixels) return;

    for (int row = 0; row < sprite->h; row++) {
        int py = y + row;
        if (py < 0 || py >= scr_h) continue;
        for (int col = 0; col < sprite->w; col++) {
            int px = x + col;
            if (px < 0 || px >= scr_w) continue;
            uint32_t pixel = sprite->pixels[row * sprite->w + col];
            if ((pixel & 0xFF000000) == 0) continue;   /* Alpha 0 = transparent */
            backbuffer[py * scr_w + px] = pixel & 0x00FFFFFF;
        }
    }
}

/* ──────── Text ──────── */

void gfx_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg, int scale) {
    if (!gfx_is_ready) return;
    if (scale < 1) scale = 1;

    const uint8_t* glyph = font8x8_basic[(uint8_t)c & 0x7F];

    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            int set = bits & (1 << col);   /* Bit 0 = leftmost pixel */
            if (!set && bg == GFX_TRANSPARENT) continue;
            uint32_t color = set ? fg : bg;
            if (scale == 1) {
                gfx_put_pixel(x + col, y + row, color);
            } else {
                gfx_fill_rect(x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}

void gfx_draw_text(int x, int y, const char* str, uint32_t fg, uint32_t bg, int scale) {
    if (scale < 1) scale = 1;
    int cx = x;
    while (*str) {
        if (*str == '\n') {
            cx = x;
            y += 8 * scale;
        } else {
            gfx_draw_char(cx, y, *str, fg, bg, scale);
            cx += 8 * scale;
        }
        str++;
    }
}

/* ──────── Presentation ──────── */

void gfx_present(void) {
    if (!gfx_is_ready) return;
    gfx_present_buffer(backbuffer);
}

void gfx_present_buffer(const uint32_t* pixels) {
    if (!fb_available() || !pixels) return;

    /* Draw into the HIDDEN page, then flip it in: tear-free. Without
     * page flipping fb_ptr_back() is just the front buffer (old path). */
    uint8_t*  dst   = (uint8_t*)fb_ptr_back();
    uint32_t  pitch = fb_pitch();
    uint32_t  w     = fb_width();
    uint32_t  h     = fb_height();

    if (pitch == w * 4) {
        /* Common case: tightly packed scanlines, single copy */
        memcpy(dst, pixels, w * h * 4);
    } else {
        for (uint32_t row = 0; row < h; row++)
            memcpy(dst + row * pitch, pixels + row * w, w * 4);
    }

    fb_flip();
}

/* ──────── Direct front-buffer rendering (boot console) ──────── */

void gfx_front_char(int px, int py, char c, uint32_t fg, uint32_t bg) {
    if (!fb_available()) return;

    uint8_t*  base  = (uint8_t*)fb_ptr();
    uint32_t  pitch = fb_pitch();

    if (px < 0 || py < 0) return;
    if ((uint32_t)(px + 8) > fb_width() || (uint32_t)(py + 8) > fb_height()) return;

    const uint8_t* glyph = font8x8_basic[(uint8_t)c & 0x7F];

    for (int row = 0; row < 8; row++) {
        uint32_t* line = (uint32_t*)(base + (uint32_t)(py + row) * pitch) + px;
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++)
            line[col] = (bits & (1 << col)) ? fg : bg;
    }
}

void gfx_front_scroll(int row_px, uint32_t bg) {
    if (!fb_available() || row_px <= 0) return;

    uint8_t*  base   = (uint8_t*)fb_ptr();
    uint32_t  pitch  = fb_pitch();
    uint32_t  h      = fb_height();
    uint32_t  w      = fb_width();

    if ((uint32_t)row_px >= h) row_px = (int)h;

    /* Move everything up by row_px scanlines */
    memcpy(base, base + (uint32_t)row_px * pitch, (h - (uint32_t)row_px) * pitch);

    /* Clear the newly exposed rows */
    for (uint32_t row = h - (uint32_t)row_px; row < h; row++) {
        uint32_t* line = (uint32_t*)(base + row * pitch);
        for (uint32_t col = 0; col < w; col++)
            line[col] = bg;
    }
}

void gfx_front_clear(uint32_t color) {
    if (!fb_available()) return;

    uint8_t*  base  = (uint8_t*)fb_ptr();
    uint32_t  pitch = fb_pitch();

    for (uint32_t row = 0; row < fb_height(); row++) {
        uint32_t* line = (uint32_t*)(base + row * pitch);
        for (uint32_t col = 0; col < fb_width(); col++)
            line[col] = color;
    }
}
