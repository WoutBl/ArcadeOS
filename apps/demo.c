/*
 * ArcadeOS – STARCATCH (SDK demo game)
 *
 * The reference game for the ArcadeOS SDK (sdk/arcade.h): steer the ship
 * with the D-pad or left stick, catch the falling stars, dodge the rocks.
 * Exercises every SDK feature: the fixed-timestep loop, input edges,
 * sprites, entities, AABB collision, canned SFX, PRNG, and save slots
 * (high score in slot 0).
 */

#include "../sdk/arcade.h"
#include "../libc/syscall.h"

/* ──────── Sprites (8x8, 0x00RRGGBB, __ = transparent) ──────── */

#define __ SURF_TRANSPARENT
#define CY 0x00FFD34Du   /* Star yellow */
#define CR 0x00E04040u   /* Rock red */
#define CW 0x00FFFFFFu   /* Ship white */
#define CB 0x006090FFu   /* Ship blue */

static const uint32_t ship_px[64] = {
    __, __, __, CW, CW, __, __, __,
    __, __, __, CW, CW, __, __, __,
    __, __, CB, CB, CB, CB, __, __,
    __, __, CB, CW, CW, CB, __, __,
    __, CB, CB, CB, CB, CB, CB, __,
    CB, CB, CB, CB, CB, CB, CB, CB,
    CB, __, CB, __, __, CB, __, CB,
    __, __, __, __, __, __, __, __,
};

static const uint32_t star_px[64] = {
    __, __, __, CY, CY, __, __, __,
    __, __, __, CY, CY, __, __, __,
    __, CY, CY, CY, CY, CY, CY, __,
    CY, CY, CY, CY, CY, CY, CY, CY,
    __, CY, CY, CY, CY, CY, CY, __,
    __, __, CY, CY, CY, CY, __, __,
    __, CY, CY, __, __, CY, CY, __,
    CY, CY, __, __, __, __, CY, CY,
};

static const uint32_t rock_px[64] = {
    __, __, CR, CR, CR, __, __, __,
    __, CR, CR, CR, CR, CR, __, __,
    CR, CR, CR, CR, CR, CR, CR, __,
    CR, CR, CR, CR, CR, CR, CR, CR,
    CR, CR, CR, CR, CR, CR, CR, CR,
    __, CR, CR, CR, CR, CR, CR, __,
    __, CR, CR, CR, CR, CR, __, __,
    __, __, CR, CR, __, __, __, __,
};

static const sprite_t spr_ship = { ship_px, 8, 8 };
static const sprite_t spr_star = { star_px, 8, 8 };
static const sprite_t spr_rock = { rock_px, 8, 8 };

/* ──────── Game state ──────── */

#define SCALE      3            /* Sprites render 24x24 */
#define MAX_FALL   8
#define KIND_STAR  0
#define KIND_ROCK  1

typedef struct { uint32_t magic; int high; } save_t;
#define SAVE_MAGIC 0x53544152u  /* 'STAR' */

static void draw_number(surface_t* s, int x, int y, int v, uint32_t color) {
    char buf[8];
    int n = 0;
    if (v <= 0) buf[n++] = '0';
    while (v > 0 && n < 7) { buf[n++] = (char)('0' + v % 10); v /= 10; }
    buf[n] = '\0';
    for (int i = 0; i < n / 2; i++) { char t = buf[i]; buf[i] = buf[n-1-i]; buf[n-1-i] = t; }
    surf_draw_text(s, x, y, buf, color, SURF_TRANSPARENT, 2);
}

static void spawn_faller(entity_t* e, int screen_w) {
    e->active = 1;
    e->kind   = (arcade_rand_range(0, 3) == 0) ? KIND_ROCK : KIND_STAR;
    e->sprite = (e->kind == KIND_ROCK) ? &spr_rock : &spr_star;
    e->w = e->h = 8 * SCALE;
    e->x = FX(arcade_rand_range(0, screen_w - e->w));
    e->y = FX(-e->h);
    e->vx = 0;
    e->vy = FX(2) + arcade_rand_range(0, 512);
}

int main(void) {
    arcade_t a;
    if (arcade_init(&a) != 0) {
        write(1, "starcatch: no framebuffer\n", 26);
        return 1;
    }

    save_t sv;
    int high = 0;
    if (arcade_load("STARCAT", 0, &sv, sizeof(sv)) == (int)sizeof(sv) &&
        sv.magic == SAVE_MAGIC)
        high = sv.high;

    entity_t ship = { 0 };
    ship.active = 1;
    ship.sprite = &spr_ship;
    ship.w = ship.h = 8 * SCALE;
    ship.x = FX(a.w / 2 - ship.w / 2);
    ship.y = FX(a.h - ship.h - 16);

    entity_t fall[MAX_FALL] = { 0 };

    int score = 0, lives = 3, game_over = 0;
    unsigned int next_spawn = 0;

    while (arcade_frame(&a)) {
        if (a.pressed & PAD_BTN_SELECT) exit(0);

        if (game_over) {
            if (a.pressed & PAD_BTN_START) {
                score = 0; lives = 3; game_over = 0;
                for (int i = 0; i < MAX_FALL; i++) fall[i].active = 0;
            }
        } else {
            /* Steer: D-pad or left stick, fixed-point velocity */
            fx_t speed = FX(5);
            ship.vx = 0;
            if (a.held & PAD_BTN_LEFT)  ship.vx = -speed;
            if (a.held & PAD_BTN_RIGHT) ship.vx =  speed;
            if (a.pad.lx < -8000) ship.vx = -speed;
            if (a.pad.lx >  8000) ship.vx =  speed;
            ship.x += ship.vx;
            if (FX_INT(ship.x) < 0)              ship.x = 0;
            if (FX_INT(ship.x) > a.w - ship.w)   ship.x = FX(a.w - ship.w);

            /* Spawn a faller every ~0.5 s */
            if (a.frame >= next_spawn) {
                for (int i = 0; i < MAX_FALL; i++) {
                    if (!fall[i].active) { spawn_faller(&fall[i], a.w); break; }
                }
                next_spawn = a.frame + 30;
            }

            /* Fall + collide */
            for (int i = 0; i < MAX_FALL; i++) {
                if (!fall[i].active) continue;
                arcade_entity_move(&fall[i]);

                if (arcade_entity_overlap(&ship, &fall[i])) {
                    fall[i].active = 0;
                    if (fall[i].kind == KIND_STAR) {
                        score++;
                        sfx_score();
                    } else {
                        lives--;
                        sfx_lose();
                        if (lives <= 0) {
                            game_over = 1;
                            sfx_gameover();
                            if (score > high) {
                                high = score;
                                sv.magic = SAVE_MAGIC;
                                sv.high  = high;
                                arcade_save("STARCAT", 0, &sv, sizeof(sv));
                            }
                        }
                    }
                } else if (FX_INT(fall[i].y) > a.h) {
                    fall[i].active = 0;
                }
            }
        }

        /* ──────── Render ──────── */
        surf_clear(&a.screen, rgb(8, 8, 24));

        for (int i = 0; i < MAX_FALL; i++)
            arcade_entity_draw(&a.screen, &fall[i], SCALE);
        if (!game_over)
            arcade_entity_draw(&a.screen, &ship, SCALE);

        surf_draw_text(&a.screen, 16, 12, "SCORE:", rgb(255,255,255), SURF_TRANSPARENT, 2);
        draw_number(&a.screen, 16 + 6*8*2, 12, score, rgb(255,255,255));
        surf_draw_text(&a.screen, a.w/2 - 60, 12, "HI:", rgb(255,211,77), SURF_TRANSPARENT, 2);
        draw_number(&a.screen, a.w/2 - 60 + 3*8*2, 12, high, rgb(255,211,77));
        surf_draw_text(&a.screen, a.w - 120, 12, "LIVES:", rgb(255,100,100), SURF_TRANSPARENT, 1);
        draw_number(&a.screen, a.w - 120 + 6*8, 10, lives, rgb(255,100,100));

        if (game_over) {
            surf_draw_text(&a.screen, a.w/2 - 4*8*4, a.h/2 - 24, "GAME OVER",
                           rgb(255,80,80), SURF_TRANSPARENT, 4);
            surf_draw_text(&a.screen, a.w/2 - 8*8, a.h/2 + 24, "START TO RESTART",
                           rgb(200,200,220), SURF_TRANSPARENT, 1);
        }
        surf_draw_text(&a.screen, 8, a.h - 12, "SELECT: QUIT", rgb(90,100,140),
                       SURF_TRANSPARENT, 1);
    }

    return 0;
}
