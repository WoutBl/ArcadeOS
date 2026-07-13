/*
 * ArcadeOS – BLASTER (twin-cluster arena shooter)
 *
 * Robotron-style: one button cluster moves, the other shoots.
 *   Move:  D-pad / left stick (pad 0)
 *   Shoot: face buttons fire in their DIAMOND direction —
 *          triangle/Y (V key) = up, cross/A (X key) = down,
 *          square/X (C key) = left, circle/B (Z key) = right.
 *          A DS4 right stick fires too (true twin-stick).
 *   SELECT quits, START pauses/restarts.
 *
 * Enemies spawn at the arena edges and home in. Each wave is faster.
 * High score persists in save slot 0 (BLASTER0.SAV).
 */

#include "../sdk/arcade.h"
#include "../libc/syscall.h"

#define MAX_SHOTS   12
#define MAX_FOES    16
#define PLAYER_SIZE 18
#define FOE_SIZE    16
#define SHOT_SIZE   6

typedef struct { uint32_t magic; int high; } save_t;
#define SAVE_MAGIC 0xB1A57E12u

static void draw_number(surface_t* s, int x, int y, int v, uint32_t color, int scale) {
    char buf[10];
    int n = 0;
    if (v <= 0) buf[n++] = '0';
    while (v > 0 && n < 9) { buf[n++] = (char)('0' + v % 10); v /= 10; }
    buf[n] = '\0';
    for (int i = 0; i < n / 2; i++) { char t = buf[i]; buf[i] = buf[n-1-i]; buf[n-1-i] = t; }
    surf_draw_text(s, x, y, buf, color, SURF_TRANSPARENT, scale);
}

static void spawn_foe(entity_t* e, int w, int h, int wave) {
    e->active = 1;
    e->w = e->h = FOE_SIZE;
    int edge = arcade_rand_range(0, 3);
    switch (edge) {
        case 0: e->x = FX(arcade_rand_range(0, w - FOE_SIZE)); e->y = FX(-FOE_SIZE); break;
        case 1: e->x = FX(arcade_rand_range(0, w - FOE_SIZE)); e->y = FX(h); break;
        case 2: e->x = FX(-FOE_SIZE); e->y = FX(arcade_rand_range(0, h - FOE_SIZE)); break;
        default: e->x = FX(w); e->y = FX(arcade_rand_range(0, h - FOE_SIZE)); break;
    }
    e->kind = wave;   /* Speed scales with the wave */
}

int main(void) {
    arcade_t a;
    if (arcade_init(&a) != 0) {
        write(1, "blaster: no framebuffer\n", 24);
        return 1;
    }

    save_t sv;
    int high = 0, high_dirty = 0;
    if (arcade_load("BLASTER", 0, &sv, sizeof(sv)) == (int)sizeof(sv) &&
        sv.magic == SAVE_MAGIC)
        high = sv.high;

    entity_t player = { 0 };
    entity_t shots[MAX_SHOTS] = { {0} };
    entity_t foes[MAX_FOES] = { {0} };

    int score = 0, lives = 3, wave = 1, kills = 0;
    int game_over = 0, paused = 0;
    int fire_cooldown = 0, hurt_flash = 0;
    unsigned int next_spawn = 0;

    player.active = 1;
    player.w = player.h = PLAYER_SIZE;
    player.x = FX(a.w / 2);
    player.y = FX(a.h / 2);

    while (arcade_frame(&a)) {
        a.score = score;
        if (a.pressed & PAD_BTN_SELECT) {
            if (high_dirty) { sv.magic = SAVE_MAGIC; sv.high = high; arcade_save("BLASTER", 0, &sv, sizeof(sv)); }
            exit(0);
        }
        if (a.pressed & PAD_BTN_START) {
            if (game_over) {
                score = 0; lives = 3; wave = 1; kills = 0; game_over = 0;
                player.x = FX(a.w / 2); player.y = FX(a.h / 2);
                for (int i = 0; i < MAX_FOES; i++) foes[i].active = 0;
                for (int i = 0; i < MAX_SHOTS; i++) shots[i].active = 0;
            } else {
                paused = !paused;
            }
        }

        if (!paused && !game_over) {
            /* Move: D-pad or stick */
            fx_t sp = FX(4);
            if (a.held & PAD_BTN_LEFT  || a.pad.lx < -8000) player.x -= sp;
            if (a.held & PAD_BTN_RIGHT || a.pad.lx >  8000) player.x += sp;
            if (a.held & PAD_BTN_UP    || a.pad.ly < -8000) player.y -= sp;
            if (a.held & PAD_BTN_DOWN  || a.pad.ly >  8000) player.y += sp;
            if (FX_INT(player.x) < 0) player.x = 0;
            if (FX_INT(player.x) > a.w - player.w) player.x = FX(a.w - player.w);
            if (FX_INT(player.y) < 40) player.y = FX(40);
            if (FX_INT(player.y) > a.h - player.h - 20) player.y = FX(a.h - player.h - 20);

            /* Shoot: face buttons = directions (or right stick on a DS4) */
            if (fire_cooldown > 0) fire_cooldown--;
            fx_t dx = 0, dy = 0;
            /* Diamond layout: each face button fires toward its
             * physical position on the pad */
            if (a.held & PAD_BTN_B) dx =  FX(9);            /* Circle (Z key): right */
            if (a.held & PAD_BTN_X) dx = -FX(9);            /* Square (C key): left */
            if (a.held & PAD_BTN_A) dy =  FX(9);            /* Cross  (X key): down */
            if (a.held & PAD_BTN_Y) dy = -FX(9);            /* Triangle (V key): up */
            if (a.pad.rx >  8000) dx =  FX(9);
            if (a.pad.rx < -8000) dx = -FX(9);
            if (a.pad.ry >  8000) dy =  FX(9);
            if (a.pad.ry < -8000) dy = -FX(9);
            if ((dx || dy) && fire_cooldown == 0) {
                for (int i = 0; i < MAX_SHOTS; i++) {
                    if (shots[i].active) continue;
                    shots[i].active = 1;
                    shots[i].w = shots[i].h = SHOT_SIZE;
                    shots[i].x = player.x + FX(player.w / 2 - SHOT_SIZE / 2);
                    shots[i].y = player.y + FX(player.h / 2 - SHOT_SIZE / 2);
                    shots[i].vx = dx; shots[i].vy = dy;
                    fire_cooldown = 7;
                    sound(1200, 20);
                    break;
                }
            }

            /* Shots fly + expire off-screen */
            for (int i = 0; i < MAX_SHOTS; i++) {
                if (!shots[i].active) continue;
                arcade_entity_move(&shots[i]);
                int sx = FX_INT(shots[i].x), sy = FX_INT(shots[i].y);
                if (sx < -SHOT_SIZE || sx > a.w || sy < -SHOT_SIZE || sy > a.h)
                    shots[i].active = 0;
            }

            /* Spawn enemies, faster each wave */
            unsigned int interval = (wave < 8) ? (50 - wave * 5) : 12;
            if (a.frame >= next_spawn) {
                for (int i = 0; i < MAX_FOES; i++) {
                    if (!foes[i].active) { spawn_foe(&foes[i], a.w, a.h, wave); break; }
                }
                next_spawn = a.frame + interval;
            }

            /* Enemies home in on the player */
            for (int i = 0; i < MAX_FOES; i++) {
                if (!foes[i].active) continue;
                fx_t fsp = FX(1) + FX(foes[i].kind) / 4;
                if (fsp > FX(3)) fsp = FX(3);
                if (foes[i].x < player.x) foes[i].x += fsp; else foes[i].x -= fsp;
                if (foes[i].y < player.y) foes[i].y += fsp; else foes[i].y -= fsp;

                /* Shot hits */
                for (int j = 0; j < MAX_SHOTS; j++) {
                    if (!shots[j].active) continue;
                    if (arcade_entity_overlap(&foes[i], &shots[j])) {
                        foes[i].active = 0;
                        shots[j].active = 0;
                        score += 10 * wave;
                        kills++;
                        if (score > high) { high = score; high_dirty = 1; }
                        sfx_explosion();   /* PCM noise burst (voice 2) */
                        sound(700 + (kills % 5) * 60, 25);
                        if (kills % 15 == 0) { wave++; sfx_select(); }
                        break;
                    }
                }
                if (!foes[i].active) continue;

                /* Player hit */
                if (arcade_entity_overlap(&foes[i], &player)) {
                    foes[i].active = 0;
                    lives--;
                    hurt_flash = 12;
                    sfx_lose();
                    if (lives <= 0) {
                        game_over = 1;
                        sfx_gameover();
                        if (high_dirty) {
                            sv.magic = SAVE_MAGIC; sv.high = high;
                            arcade_save("BLASTER", 0, &sv, sizeof(sv));
                            high_dirty = 0;
                        }
                    }
                }
            }
            if (hurt_flash > 0) hurt_flash--;
        }

        /* ──────── Render ──────── */
        surf_clear(&a.screen, hurt_flash ? rgb(60, 8, 8) : rgb(10, 8, 20));

        /* Arena border */
        surf_draw_rect(&a.screen, 4, 36, a.w - 8, a.h - 56, rgb(50, 40, 90));

        for (int i = 0; i < MAX_SHOTS; i++)
            if (shots[i].active)
                surf_fill_rect(&a.screen, FX_INT(shots[i].x), FX_INT(shots[i].y),
                               SHOT_SIZE, SHOT_SIZE, rgb(255, 240, 120));

        for (int i = 0; i < MAX_FOES; i++)
            if (foes[i].active)
                surf_fill_rect(&a.screen, FX_INT(foes[i].x), FX_INT(foes[i].y),
                               FOE_SIZE, FOE_SIZE, rgb(230, 60, 90));

        if (!game_over) {
            surf_fill_rect(&a.screen, FX_INT(player.x), FX_INT(player.y),
                           PLAYER_SIZE, PLAYER_SIZE, rgb(90, 220, 255));
            surf_fill_rect(&a.screen, FX_INT(player.x) + 5, FX_INT(player.y) + 5,
                           PLAYER_SIZE - 10, PLAYER_SIZE - 10, rgb(255, 255, 255));
        }

        surf_draw_text(&a.screen, 16, 12, "SCORE:", rgb(255,255,255), SURF_TRANSPARENT, 2);
        draw_number(&a.screen, 16 + 6*8*2, 12, score, rgb(255,255,255), 2);
        surf_draw_text(&a.screen, a.w/2 - 52, 12, "HI:", rgb(255,211,77), SURF_TRANSPARENT, 2);
        draw_number(&a.screen, a.w/2, 12, high, rgb(255,211,77), 2);
        surf_draw_text(&a.screen, a.w - 190, 12, "WAVE", rgb(160,140,255), SURF_TRANSPARENT, 2);
        draw_number(&a.screen, a.w - 110, 12, wave, rgb(160,140,255), 2);
        surf_draw_text(&a.screen, a.w - 70, 16, "x", rgb(255,100,100), SURF_TRANSPARENT, 1);
        draw_number(&a.screen, a.w - 56, 12, lives, rgb(255,100,100), 2);

        if (game_over) {
            surf_draw_text(&a.screen, a.w/2 - 7*8*3/2, a.h/2 - 72, "BLASTER",
                           rgb(160,140,255), SURF_TRANSPARENT, 3);
            surf_draw_text(&a.screen, a.w/2 - 4*8*4 - 16, a.h/2 - 24, "GAME OVER",
                           rgb(255,80,80), SURF_TRANSPARENT, 4);
            surf_draw_text(&a.screen, a.w/2 - 8*8, a.h/2 + 24, "START TO RESTART",
                           rgb(200,200,220), SURF_TRANSPARENT, 1);
        } else if (paused) {
            surf_draw_text(&a.screen, a.w/2 - 3*8*4/2 - 24, a.h/2 - 16, "PAUSED",
                           rgb(255,220,80), SURF_TRANSPARENT, 4);
        }

        surf_draw_text(&a.screen, 8, a.h - 14, "MOVE: ARROWS  FIRE: V=UP X=DOWN C=LEFT Z=RIGHT  SELECT: QUIT",
                       rgb(90, 100, 140), SURF_TRANSPARENT, 1);
    }

    return 0;
}
