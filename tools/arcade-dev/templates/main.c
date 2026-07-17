/*
 * @TITLE@ — an ArcadeOS game.
 *
 * Built with the libarcade SDK (see sdk/README.md in the ArcadeOS
 * repo for the full API). The loop below is the standard skeleton:
 * fixed timestep, edge-detected input, whole-frame drawing.
 *
 * Controls: arrows move (walls block you — see level.h), walk into
 * the gold pickups to score, SELECT/B quits.
 * Build & run:  arcade run .
 */

#include "arcade.h"
#include "syscall.h"     /* exit() */
#include "sprites.h"
#include "level.h"

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
    fx_t x = FX(48), y = FX(48);   /* Tile (1,1) — inside the border */

    /* Editable working copy of the level (the original is in read-only
     * memory — W^X would fault a write to it) */
    static uint8_t cells[sizeof(level1_cells)];
    for (unsigned i = 0; i < sizeof(cells); i++) cells[i] = level1_cells[i];
    tilemap_t map = { cells, level1_map.w, level1_map.h, level1_map.tile };

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
        /* Move one axis at a time so walls stop you cleanly */
        fx_t nx = x, ny = y;
        if (a.held & PAD_BTN_LEFT)  nx -= FX(4);
        if (a.held & PAD_BTN_RIGHT) nx += FX(4);
        if (!arcade_tilemap_hits(&map, LEVEL1_SOLID,
                                 FX_INT(nx), FX_INT(y), 32, 32))
            x = nx;
        if (a.held & PAD_BTN_UP)   ny -= FX(4);
        if (a.held & PAD_BTN_DOWN) ny += FX(4);
        if (!arcade_tilemap_hits(&map, LEVEL1_SOLID,
                                 FX_INT(x), FX_INT(ny), 32, 32))
            y = ny;

        if (a.pressed & PAD_BTN_A) sfx_hit();

        /* Walk over a pickup tile (4): collect it */
        {
            int cx = FX_INT(x) + 16, cy = FX_INT(y) + 16;
            if (arcade_tile_at(&map, cx, cy) == 4) {
                cells[(cy / map.tile) * map.w + (cx / map.tile)] = 0;
                score++;
                sfx_score();
            }
        }

        /* ── Draw ── */
        surf_clear(&a.screen, rgb(10, 12, 34));
        arcade_draw_tilemap(&a.screen, &map, level1_colors);
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
                       "ARROWS: MOVE  GOLD: COLLECT  SELECT: QUIT",
                       rgb(90, 100, 150), SURF_TRANSPARENT, 1);
    }
    return 0;
}
