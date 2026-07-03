/*
 * ArcadeOS – User-Space Console SDK implementation
 */

#include "console.h"
#include "../include/font8x8.h"

/* ──────── Syscall wrappers (int 0x80, numbers in syscall.h) ──────── */

int gfx_info(gfx_info_t* info) {
    int ret;
    asm volatile("int $0x80" : "=a"(ret) : "a"(21), "b"(info) : "memory");
    return ret;
}

int gfx_present(const uint32_t* pixels) {
    int ret;
    asm volatile("int $0x80" : "=a"(ret) : "a"(22), "b"(pixels) : "memory");
    return ret;
}

int pad_read(int index, pad_state_t* state) {
    int ret;
    asm volatile("int $0x80" : "=a"(ret) : "a"(23), "b"(index), "c"(state) : "memory");
    return ret;
}

unsigned int ticks(void) {
    unsigned int ret;
    asm volatile("int $0x80" : "=a"(ret) : "a"(24) : "memory");
    return ret;
}

void msleep(unsigned int ms) {
    asm volatile("int $0x80" : : "a"(25), "b"(ms) : "memory");
}

void sound(int freq_hz, int ms) {
    asm volatile("int $0x80" : : "a"(29), "b"(freq_hz), "c"(ms) : "memory");
}

void report_score(int score) {
    asm volatile("int $0x80" : : "a"(30), "b"(score) : "memory");
}

int readdir_at(const char* path, int index, dirent_info_t* out) {
    int ret;
    asm volatile("int $0x80" : "=a"(ret)
                 : "a"(26), "b"(path), "c"(index), "d"(out) : "memory");
    return ret;
}

int save_data(const char* name, const void* buf, int len) {
    int ret;
    asm volatile("int $0x80" : "=a"(ret)
                 : "a"(27), "b"(name), "c"(buf), "d"(len) : "memory");
    return ret;
}

int load_data(const char* name, void* buf, int maxlen) {
    int ret;
    asm volatile("int $0x80" : "=a"(ret)
                 : "a"(28), "b"(name), "c"(buf), "d"(maxlen) : "memory");
    return ret;
}

/* ──────── Software drawing helpers ──────── */

void surf_clear(surface_t* s, uint32_t color) {
    int count = s->w * s->h;
    for (int i = 0; i < count; i++)
        s->pixels[i] = color;
}

void surf_fill_rect(surface_t* s, int x, int y, int w, int h, uint32_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    if (w <= 0 || h <= 0) return;

    for (int row = y; row < y + h; row++) {
        uint32_t* dst = &s->pixels[row * s->w + x];
        for (int col = 0; col < w; col++)
            dst[col] = color;
    }
}

void surf_draw_rect(surface_t* s, int x, int y, int w, int h, uint32_t color) {
    surf_fill_rect(s, x,         y,         w, 1, color);
    surf_fill_rect(s, x,         y + h - 1, w, 1, color);
    surf_fill_rect(s, x,         y,         1, h, color);
    surf_fill_rect(s, x + w - 1, y,         1, h, color);
}

void surf_draw_char(surface_t* s, int x, int y, char c, uint32_t fg, uint32_t bg, int scale) {
    if (scale < 1) scale = 1;
    const uint8_t* glyph = font8x8_basic[(uint8_t)c & 0x7F];

    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            int set = bits & (1 << col);
            if (!set && bg == SURF_TRANSPARENT) continue;
            uint32_t color = set ? fg : bg;
            surf_fill_rect(s, x + col * scale, y + row * scale, scale, scale, color);
        }
    }
}

void surf_draw_text(surface_t* s, int x, int y, const char* str, uint32_t fg, uint32_t bg, int scale) {
    if (scale < 1) scale = 1;
    int cx = x;
    while (*str) {
        if (*str == '\n') {
            cx = x;
            y += 8 * scale;
        } else {
            surf_draw_char(s, cx, y, *str, fg, bg, scale);
            cx += 8 * scale;
        }
        str++;
    }
}
