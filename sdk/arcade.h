#ifndef ARCADE_H
#define ARCADE_H

/*
 * ArcadeOS Game SDK (libarcade)
 *
 * The framework layer for writing ArcadeOS games. Sits on top of the
 * syscall libc (libc/console.h) and adds everything the built-in games
 * had to hand-roll: a fixed-timestep game loop, input edge detection,
 * fixed-point math, sprites, entities, AABB collision, save slots,
 * canned sound effects, and a PRNG.
 *
 * Quick start (see sdk/README.md for the full template):
 *
 *     #include "arcade.h"
 *     int main(void) {
 *         arcade_t a;
 *         if (arcade_init(&a) != 0) return 1;
 *         while (arcade_frame(&a)) {
 *             if (a.pressed & PAD_BTN_START) { ... }
 *             surf_clear(&a.screen, rgb(8, 10, 30));
 *             ...draw...
 *         }
 *         return 0;
 *     }
 */

#include "../libc/console.h"

/* ──────── Fixed-point math (24.8, like the built-in games) ──────── */

typedef int32_t fx_t;
#define FX(v)        ((fx_t)((v) << 8))       /* int → fixed */
#define FX_INT(v)    ((int)((v) >> 8))        /* fixed → int (floor) */
#define FX_MUL(a, b) ((fx_t)(((int64_t)(a) * (b)) >> 8))

/* ──────── Sprites (0x00RRGGBB pixels, SURF_TRANSPARENT = skip) ──────── */

typedef struct {
    const uint32_t* pixels;
    int w, h;
} sprite_t;

void arcade_draw_sprite(surface_t* s, const sprite_t* spr, int x, int y, int scale);

/* ──────── Entities: position/velocity in fixed-point ──────── */

typedef struct {
    fx_t x, y;           /* Position (fixed-point pixels) */
    fx_t vx, vy;         /* Velocity per frame */
    int  w, h;           /* Size in pixels (collision box) */
    int  active;
    int  kind;           /* Game-defined tag */
    const sprite_t* sprite;   /* Optional (NULL = game draws it itself) */
} entity_t;

/* x += vx, y += vy */
void arcade_entity_move(entity_t* e);

/* Move and bounce off the screen edges (inverts velocity). Returns a
 * bitmask of edges hit: 1=left 2=right 4=top 8=bottom. */
int arcade_entity_bounce(entity_t* e, int screen_w, int screen_h);

/* Axis-aligned overlap test between two active entities */
int arcade_entity_overlap(const entity_t* a, const entity_t* b);

/* Raw AABB test (pixel coordinates) */
int arcade_aabb(int x1, int y1, int w1, int h1,
                int x2, int y2, int w2, int h2);

/* Draw e->sprite at the entity position (no-op when sprite is NULL) */
void arcade_entity_draw(surface_t* s, const entity_t* e, int scale);

/* ──────── Save slots ──────── */

/* Whole-file save/load in slot 0-9. `game` is at most 7 characters
 * (8.3 names: the slot digit is appended, e.g. "SNAKE" slot 0 →
 * "SNAKE0.SAV"). Returns 0 / bytes read, -1 on failure. */
int arcade_save(const char* game, int slot, const void* buf, int len);
int arcade_load(const char* game, int slot, void* buf, int maxlen);

/* ──────── Sound effects ──────── */

/* Canned SFX so games sound consistent without tuning frequencies */
static inline void sfx_move(void)     { sound(600, 30);  }   /* Menu blip */
static inline void sfx_select(void)   { sound(900, 80);  }   /* Confirm */
static inline void sfx_hit(void)      { sound(440, 40);  }   /* Ball/paddle */
static inline void sfx_score(void)    { sound(880, 60);  }   /* Point up */
static inline void sfx_lose(void)     { sound(160, 300); }   /* Life lost */
static inline void sfx_gameover(void) { sound(150, 400); }

/* ──────── PRNG (xorshift32, seeded from the clock) ──────── */

void     arcade_srand(uint32_t seed);
uint32_t arcade_rand(void);
/* Uniform integer in [lo, hi] */
int      arcade_rand_range(int lo, int hi);

/* ──────── The game context ──────── */

#define ARCADE_MAX_W 640
#define ARCADE_MAX_H 480

typedef struct {
    surface_t    screen;     /* Draw into this every frame */
    int          w, h;       /* Screen size in pixels */

    pad_state_t  pad;        /* Raw pad 0 state this frame */
    uint16_t     pressed;    /* Buttons that went down THIS frame */
    uint16_t     released;   /* Buttons that went up THIS frame */
    uint16_t     held;       /* Buttons currently down */

    unsigned int frame;      /* Frame counter */
    unsigned int frame_ms;   /* Fixed timestep (default 16 ≈ 60 FPS) */
} arcade_t;

/* Initialize graphics + input + PRNG. Returns 0 on success. */
int arcade_init(arcade_t* a);

/* End-of-frame: present the screen, pace to the fixed timestep, then
 * poll the pad and compute the pressed/released edges for the NEXT
 * iteration. Always returns 1 (loop forever; games exit via exit()). */
int arcade_frame(arcade_t* a);

#endif /* ARCADE_H */
