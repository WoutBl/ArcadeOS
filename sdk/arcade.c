/*
 * ArcadeOS Game SDK – implementation (see arcade.h)
 */

#include "arcade.h"
#include "../libc/string.h"

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
    a->pressed2  = 0;
    a->released2 = 0;
    a->held2     = 0;
    a->frame    = 0;
    a->frame_ms = 16;                 /* ~60 FPS */
    a->score    = 0;

    pad_read(0, &a->pad);
    a->held = a->pad.buttons;
    pad_read(1, &a->pad2);
    a->held2 = a->pad2.buttons;

    arcade_srand(ticks() | 1);
    return 0;
}

static int last_reported_score = -1;

int arcade_frame(arcade_t* a) {
    gfx_present(framebuf);

    if (a->score != last_reported_score) {
        report_score(a->score);
        last_reported_score = a->score;
    }

    msleep(a->frame_ms);
    a->frame++;

    uint16_t prev = a->pad.buttons;
    pad_read(0, &a->pad);
    a->held     = a->pad.buttons;
    a->pressed  = (uint16_t)(a->pad.buttons & ~prev);
    a->released = (uint16_t)(prev & ~a->pad.buttons);

    uint16_t prev2 = a->pad2.buttons;
    pad_read(1, &a->pad2);
    a->held2     = a->pad2.buttons;
    a->pressed2  = (uint16_t)(a->pad2.buttons & ~prev2);
    a->released2 = (uint16_t)(prev2 & ~a->pad2.buttons);

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

/* ──────── PCM explosion (see arcade.h) ──────── */

void sfx_explosion(void) {
    static int16_t clip[4096];
    static int generated = 0;

    if (!generated) {
        /* Decaying white noise with a touch of low-frequency rumble.
         * Private xorshift: must not disturb the game's PRNG stream. */
        uint32_t r = 0x9E3779B9u;
        for (int i = 0; i < 4096; i++) {
            r ^= r << 13; r ^= r >> 17; r ^= r << 5;
            int32_t noise  = (int16_t)(r & 0xFFFF) / 3;
            int32_t rumble = ((i / 60) & 1) ? 2500 : -2500;
            int32_t s      = noise + rumble;
            clip[i] = (int16_t)(s * (4096 - i) / 4096);   /* Linear decay */
        }
        generated = 1;
    }
    sfx_pcm(2, clip, 4096, 11025, 220);
}

/* ──────── Player-select screen (see arcade.h) ──────── */

int arcade_choose_players(arcade_t* a, const char* title, unsigned flags) {
    char p1[SESSION_NAME_LEN], p2[SESSION_NAME_LEN];
    int  nplayers = arcade_session(p1, p2);
    if (p1[0] == '\0') { p1[0] = 'P'; p1[1] = '1'; p1[2] = '\0'; }

    /* Build the entry list */
    const char* labels[4];
    int         modes[4];
    int n = 0;
    labels[n] = "1 PLAYER  (VS CPU)";     modes[n++] = ARCADE_MODE_1P;
    labels[n] = "2 PLAYERS (LOCAL)";      modes[n++] = ARCADE_MODE_2P;
    if (flags & ARCADE_CHOOSE_NET) {
        labels[n] = "HOST ONLINE GAME";   modes[n++] = ARCADE_MODE_NET_HOST;
        labels[n] = "JOIN ONLINE GAME";   modes[n++] = ARCADE_MODE_NET_JOIN;
    }

    int sel = 0;
    while (arcade_frame(a)) {
        if (a->pressed & (PAD_BTN_B | PAD_BTN_SELECT)) return ARCADE_MODE_QUIT;
        if (a->pressed & PAD_BTN_DOWN) { sel = (sel + 1) % n; sfx_move(); }
        if (a->pressed & PAD_BTN_UP)   { sel = (sel + n - 1) % n; sfx_move(); }
        if (a->pressed & (PAD_BTN_A | PAD_BTN_START)) {
            sfx_select();
            return modes[sel];
        }

        surface_t* s = &a->screen;
        surf_clear(s, rgb(8, 10, 30));
        surf_draw_text(s, a->w / 2 - (int)strlen(title) * 12, 60, title,
                       rgb(255, 255, 255), SURF_TRANSPARENT, 3);

        /* Who is signed in */
        {
            char line[48] = "P1 ";
            strcpy(line + 3, p1);
            if (nplayers == 2 && p2[0]) {
                int l = (int)strlen(line);
                strcpy(line + l, "   P2 ");
                strcpy(line + l + 6, p2);
            }
            surf_draw_text(s, a->w / 2 - (int)strlen(line) * 4, 104, line,
                           rgb(120, 140, 220), SURF_TRANSPARENT, 1);
        }

        int y = 150;
        for (int i = 0; i < n; i++) {
            if (i == sel) {
                surf_fill_rect(s, a->w / 2 - 180, y - 8, 360, 36, rgb(30, 50, 130));
                surf_draw_rect(s, a->w / 2 - 180, y - 8, 360, 36, rgb(120, 160, 255));
            }
            surf_draw_text(s, a->w / 2 - (int)strlen(labels[i]) * 8, y, labels[i],
                           i == sel ? rgb(255, 255, 255) : rgb(140, 150, 190),
                           SURF_TRANSPARENT, 2);
            y += 46;
        }

        surf_draw_text(s, a->w / 2 - 132, a->h - 28,
                       "A(X): START   B(Z): BACK TO MENU",
                       rgb(120, 140, 220), SURF_TRANSPARENT, 1);
    }
    return ARCADE_MODE_QUIT;
}
