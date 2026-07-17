/*
 * @TITLE@ — level.
 *
 * Design your own with the level editor (`arcade levels`) and paste
 * its EXPORT C output here. Draw with arcade_draw_tilemap(); block
 * movement with arcade_tilemap_hits() and the _SOLID mask.
 */
#ifndef LEVEL_H
#define LEVEL_H

#include "arcade.h"

/* level1: 20x15 tiles @ 32px — ArcadeOS level editor */
static const uint8_t level1_cells[300] = {
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,0,0,0,0,4,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
    1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
    1,0,0,2,2,2,0,0,0,0,0,0,0,0,2,2,0,0,0,1,
    1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,0,0,0,1,
    1,0,0,0,0,0,0,0,4,0,0,0,0,0,0,2,0,0,0,1,
    1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
    1,0,0,0,0,0,0,2,2,2,2,2,0,0,0,0,0,0,0,1,
    1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,0,1,
    1,0,0,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
    1,0,0,0,0,0,2,0,0,0,0,0,0,2,2,0,0,0,0,1,
    1,0,0,0,0,0,2,0,0,0,0,0,0,0,0,0,0,0,0,1,
    1,0,0,0,0,0,2,0,0,0,0,4,0,0,0,0,0,0,0,1,
    1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
};
static const tilemap_t level1_map = { level1_cells, 20, 15, 32 };
static const uint32_t level1_colors[16] = {
    0x000000,0x3C64FF,0x78DC96,0xE05050,0xFFD37E,0xB478FF,0x50C8C8,0xFFDC50,
    0x1B2450,0xFF9078,0xC8C8D2,0x787878,0xA05028,0xF0A0C8,0xFFFFFF,0x0A4A20,
};
#define LEVEL1_SOLID 0x6u  /* arcade_tilemap_hits mask */

#endif /* LEVEL_H */
