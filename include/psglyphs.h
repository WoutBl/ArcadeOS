#ifndef PSGLYPHS_H
#define PSGLYPHS_H

#include "console_abi.h"

/*
 * ArcadeOS – PlayStation-style face-button glyphs (Cross/Circle/Square/
 * Triangle). Shared pixel data for the kernel overlay renderer (src/fb.c)
 * and the game SDK (sdk/arcade.c), so both sides draw the identical icon.
 * Matches the DS4 decode in src/usb.c: A=Cross, B=Circle, X=Square,
 * Y=Triangle.
 */

#define PS_GLYPH_SIZE 9

typedef struct {
    const char* rows[PS_GLYPH_SIZE];
    unsigned int color;
} ps_glyph_t;

static const ps_glyph_t PS_GLYPH_CROSS = {
    {
        ".........",
        ".#.....#.",
        "..#...#..",
        "...#.#...",
        "....#....",
        "...#.#...",
        "..#...#..",
        ".#.....#.",
        ".........",
    }, 0x4EA8F5
};

static const ps_glyph_t PS_GLYPH_CIRCLE = {
    {
        "...###...",
        "..#...#..",
        ".#.....#.",
        "#.......#",
        "#.......#",
        "#.......#",
        ".#.....#.",
        "..#...#..",
        "...###...",
    }, 0xE85B5B
};

static const ps_glyph_t PS_GLYPH_SQUARE = {
    {
        "#########",
        "#.......#",
        "#.......#",
        "#.......#",
        "#.......#",
        "#.......#",
        "#.......#",
        "#.......#",
        "#########",
    }, 0xE86BB8
};

static const ps_glyph_t PS_GLYPH_TRIANGLE = {
    {
        "....#....",
        "...#.#...",
        "..#...#..",
        "..#...#..",
        ".#.....#.",
        ".#.....#.",
        "#.......#",
        "#########",
        ".........",
    }, 0x4CD98A
};

/* static inline: unlike a plain `static` function, GCC never warns on an
 * unused one, so a TU that only needs Cross/Circle doesn't get flagged
 * for the Square/Triangle branches it never takes. */
static inline const ps_glyph_t* ps_glyph_for_button(unsigned int btn) {
    switch (btn) {
        case PAD_BTN_A: return &PS_GLYPH_CROSS;
        case PAD_BTN_B: return &PS_GLYPH_CIRCLE;
        case PAD_BTN_X: return &PS_GLYPH_SQUARE;
        case PAD_BTN_Y: return &PS_GLYPH_TRIANGLE;
        default:        return 0;
    }
}

#endif /* PSGLYPHS_H */
