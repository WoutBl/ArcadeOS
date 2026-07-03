#ifndef LIBC_CONSOLE_H
#define LIBC_CONSOLE_H

/*
 * ArcadeOS – User-Space Console SDK
 *
 * Everything a game needs: framebuffer access, controller input,
 * frame timing, and simple software drawing helpers that render into
 * the game's own pixel buffer (presented with gfx_present()).
 */

#include <stdint.h>
#include "../include/console_abi.h"

/* ──────── Syscall wrappers ──────── */

/* Query framebuffer geometry. Returns 0 on success. */
int gfx_info(gfx_info_t* info);

/* Blit a width*height 0x00RRGGBB buffer to the screen. */
int gfx_present(const uint32_t* pixels);

/* Read controller state (index 0 = keyboard-mapped pad). */
int pad_read(int index, pad_state_t* state);

/* Milliseconds since boot. */
unsigned int ticks(void);

/* Sleep for ms milliseconds (kernel yields the CPU). */
void msleep(unsigned int ms);

/* List a directory. Returns 0 on success, -1 past the end. */
int readdir_at(const char* path, int index, dirent_info_t* out);

/* Play a square-wave tone: freq in Hz (0 stops any playing sound),
 * duration in ms. Routed to AC97 PCM or the PC speaker by the kernel. */
void sound(int freq_hz, int ms);

/* Save data ("memory card"): whole-file write/read on the game volume.
 * name is a bare 8.3 filename, e.g. "SNAKE.SAV".
 * save_data returns 0 on success; load_data returns bytes read or -1. */
int save_data(const char* name, const void* buf, int len);
int load_data(const char* name, void* buf, int maxlen);

/* ──────── Software drawing helpers (operate on the game's buffer) ──────── */

typedef struct {
    uint32_t* pixels;
    int       w;
    int       h;
} surface_t;

static inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

void surf_clear(surface_t* s, uint32_t color);
void surf_fill_rect(surface_t* s, int x, int y, int w, int h, uint32_t color);
void surf_draw_rect(surface_t* s, int x, int y, int w, int h, uint32_t color);

/* 8x8 font, integer scale. bg = SURF_TRANSPARENT skips background pixels. */
#define SURF_TRANSPARENT 0xFF000000u
void surf_draw_char(surface_t* s, int x, int y, char c, uint32_t fg, uint32_t bg, int scale);
void surf_draw_text(surface_t* s, int x, int y, const char* str, uint32_t fg, uint32_t bg, int scale);

#endif /* LIBC_CONSOLE_H */
