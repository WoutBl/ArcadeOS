/*
 * @TITLE@ — an ArcadeOS game.
 *
 * Built with the libarcade SDK (see sdk/README.md in the ArcadeOS
 * repo for the full API). The loop below is the standard skeleton:
 * fixed timestep, edge-detected input, whole-frame drawing.
 *
 * Controls here: arrows move, A (X key) beeps, SELECT/B quits.
 * Build & run:  arcade run .
 */

#include "arcade.h"
#include "syscall.h"     /* exit() */
#include "sprites.h"

#define SAVE_MAGIC 0x40474D45u   /* Bump this when save_t changes */
typedef struct { unsigned int magic; int high; } save_t;

int main(void) {
    arcade_t a;
    if (arcade_init(&a) != 0) return 1;

    /* Persistent high score (slot 0 on the game volume) */
    save_t sv;
    int high = 0;
    if (arcade_load("@NAME@", 0, &sv, sizeof(sv)) == (int)sizeof(sv) &&
        sv.magic == SAVE_MAGIC)
        high = sv.high;

    int score = 0;
    fx_t x = FX(a.w / 2 - 8), y = FX(a.h / 2 - 8);

    while (arcade_frame(&a)) {
        a.score = score;             /* Live score for the REST API */

        /* ── Input ── */
        if (a.pressed & (PAD_BTN_SELECT | PAD_BTN_B)) {
            if (score > high) {
                sv.magic = SAVE_MAGIC;
                sv.high  = score;
                arcade_save("@NAME@", 0, &sv, sizeof(sv));
            }
            exit(0);                 /* Back to the launcher */
        }
        if (a.held & PAD_BTN_LEFT)  x -= FX(4);
        if (a.held & PAD_BTN_RIGHT) x += FX(4);
        if (a.held & PAD_BTN_UP)    y -= FX(4);
        if (a.held & PAD_BTN_DOWN)  y += FX(4);
        if (a.pressed & PAD_BTN_A) { score++; sfx_score(); }

        /* Keep the hero on screen */
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x > FX(a.w - 32)) x = FX(a.w - 32);
        if (y > FX(a.h - 32)) y = FX(a.h - 32);

        /* ── Draw ── */
        surf_clear(&a.screen, rgb(10, 12, 34));
        surf_draw_text(&a.screen, 16, 16, "@TITLE@",
                       rgb(255, 255, 255), SURF_TRANSPARENT, 2);
        {
            char buf[16] = "SCORE ";
            int v = score, n = 6;
            char tmp[8]; int t = 0;
            if (v == 0) tmp[t++] = '0';
            while (v > 0 && t < 7) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
            while (t > 0) buf[n++] = tmp[--t];
            buf[n] = '\0';
            surf_draw_text(&a.screen, 16, 40, buf,
                           rgb(255, 220, 80), SURF_TRANSPARENT, 1);
        }
        arcade_draw_sprite(&a.screen, &spr_hero, FX_INT(x), FX_INT(y), 4);
        surf_draw_text(&a.screen, 16, a.h - 20,
                       "ARROWS: MOVE  A(X): SCORE  SELECT: QUIT",
                       rgb(90, 100, 150), SURF_TRANSPARENT, 1);
    }
    return 0;
}
