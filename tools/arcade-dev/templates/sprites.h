/*
 * @TITLE@ — sprites.
 *
 * Paint your own with the sprite editor (`arcade sprites`), then
 * paste its exported C here. _ = SURF_TRANSPARENT (skipped pixels).
 */
#ifndef SPRITES_H
#define SPRITES_H

#include "arcade.h"

#define _ SURF_TRANSPARENT

/* An 8x8 hero (draw over me!) */
static const uint32_t hero_px[64] = {
    _,        _,        0xFFD37E, 0xFFD37E, 0xFFD37E, 0xFFD37E, _,        _,
    _,        0xFFD37E, 0xFFD37E, 0xFFD37E, 0xFFD37E, 0xFFD37E, 0xFFD37E, _,
    0xFFD37E, 0x1B2450, 0xFFFFFF, 0xFFD37E, 0xFFD37E, 0x1B2450, 0xFFFFFF, 0xFFD37E,
    0xFFD37E, 0xFFD37E, 0xFFD37E, 0xFFD37E, 0xFFD37E, 0xFFD37E, 0xFFD37E, 0xFFD37E,
    0xFFD37E, 0xE05050, 0xE05050, 0xE05050, 0xE05050, 0xE05050, 0xE05050, 0xFFD37E,
    _,        0xFFD37E, 0xFFD37E, 0xFFD37E, 0xFFD37E, 0xFFD37E, 0xFFD37E, _,
    _,        _,        0x3C64FF, 0x3C64FF, 0x3C64FF, 0x3C64FF, _,        _,
    _,        _,        0x3C64FF, _,        _,        0x3C64FF, _,        _,
};
static const sprite_t spr_hero = { hero_px, 8, 8 };

#undef _
#endif /* SPRITES_H */
