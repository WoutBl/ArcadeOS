/*
 * ArcadeOS Game SDK – implementation (see arcade.h)
 */

#include "arcade.h"

/* ──────── Framebuffer (static: user apps have no heap) ──────── */

static uint32_t framebuf[ARCADE_MAX_W * ARCADE_MAX_H];

/* ──────── PRNG ──────── */

static uint32_t rng_state = 0x12345678;

void arcade_srand(uint32_t seed) {
    rng_state = seed ? seed : 0x12345678;
}

uint32_t arcade_rand(void) {
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

int arcade_rand_range(int lo, int hi) {
    if (hi <= lo) return lo;
    return lo + (int)(arcade_rand() % (uint32_t)(hi - lo + 1));
}

/* ──────── Game loop ──────── */

int arcade_init(arcade_t* a) {
    gfx_info_t info;
    if (gfx_info(&info) != 0) return -1;
    if (info.width > ARCADE_MAX_W || info.height > ARCADE_MAX_H) return -1;

    a->w = (int)info.width;
    a->h = (int)info.height;
    a->screen.pixels = framebuf;
    a->screen.w = a->w;
    a->screen.h = a->h;

    a->pressed  = 0;
    a->released = 0;
    a->held     = 0;
    a->frame    = 0;
    a->frame_ms = 16;                 /* ~60 FPS */

    pad_read(0, &a->pad);
    a->held = a->pad.buttons;

    arcade_srand(ticks() | 1);
    return 0;
}

int arcade_frame(arcade_t* a) {
    gfx_present(framebuf);

    msleep(a->frame_ms);
    a->frame++;

    uint16_t prev = a->pad.buttons;
    pad_read(0, &a->pad);
    a->held     = a->pad.buttons;
    a->pressed  = (uint16_t)(a->pad.buttons & ~prev);
    a->released = (uint16_t)(prev & ~a->pad.buttons);

    return 1;
}

/* ──────── Sprites ──────── */

void arcade_draw_sprite(surface_t* s, const sprite_t* spr, int x, int y, int scale) {
    if (!spr || !spr->pixels || scale < 1) return;
    for (int sy = 0; sy < spr->h; sy++) {
        for (int sx = 0; sx < spr->w; sx++) {
            uint32_t c = spr->pixels[sy * spr->w + sx];
            if (c == SURF_TRANSPARENT) continue;
            surf_fill_rect(s, x + sx * scale, y + sy * scale, scale, scale, c);
        }
    }
}

/* ──────── Entities ──────── */

void arcade_entity_move(entity_t* e) {
    if (!e->active) return;
    e->x += e->vx;
    e->y += e->vy;
}

int arcade_entity_bounce(entity_t* e, int screen_w, int screen_h) {
    if (!e->active) return 0;
    int hit = 0;
    e->x += e->vx;
    e->y += e->vy;

    if (FX_INT(e->x) < 0)                 { e->x = 0; e->vx = -e->vx; hit |= 1; }
    if (FX_INT(e->x) > screen_w - e->w)   { e->x = FX(screen_w - e->w); e->vx = -e->vx; hit |= 2; }
    if (FX_INT(e->y) < 0)                 { e->y = 0; e->vy = -e->vy; hit |= 4; }
    if (FX_INT(e->y) > screen_h - e->h)   { e->y = FX(screen_h - e->h); e->vy = -e->vy; hit |= 8; }
    return hit;
}

int arcade_aabb(int x1, int y1, int w1, int h1,
                int x2, int y2, int w2, int h2) {
    return x1 < x2 + w2 && x1 + w1 > x2 &&
           y1 < y2 + h2 && y1 + h1 > y2;
}

int arcade_entity_overlap(const entity_t* a, const entity_t* b) {
    if (!a->active || !b->active) return 0;
    return arcade_aabb(FX_INT(a->x), FX_INT(a->y), a->w, a->h,
                       FX_INT(b->x), FX_INT(b->y), b->w, b->h);
}

void arcade_entity_draw(surface_t* s, const entity_t* e, int scale) {
    if (!e->active || !e->sprite) return;
    arcade_draw_sprite(s, e->sprite, FX_INT(e->x), FX_INT(e->y), scale);
}

/* ──────── Save slots ──────── */

static int slot_name(const char* game, int slot, char out[13]) {
    int n = 0;
    while (game[n] && n < 7) {
        char c = game[n];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        out[n] = c;
        n++;
    }
    if (n == 0 || slot < 0 || slot > 9) return -1;
    out[n++] = (char)('0' + slot);
    out[n++] = '.';
    out[n++] = 'S';
    out[n++] = 'A';
    out[n++] = 'V';
    out[n]   = '\0';
    return 0;
}

int arcade_save(const char* game, int slot, const void* buf, int len) {
    char name[13];
    if (slot_name(game, slot, name) != 0) return -1;
    return save_data(name, buf, len);
}

int arcade_load(const char* game, int slot, void* buf, int maxlen) {
    char name[13];
    if (slot_name(game, slot, name) != 0) return -1;
    return load_data(name, buf, maxlen);
}
