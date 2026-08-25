#ifndef GAMEMETA_H
#define GAMEMETA_H

/*
 * ArcadeOS – game display-title trailer
 *
 * Every packaged game ELF gets 32 trailing bytes appended by
 * tools/pack_title.py at build/deploy time: a magic + the game's real
 * display title, pulled from an ARCADE_GAME("...") declaration in its
 * source (sdk/arcade.h). ELF loaders only look at the program headers,
 * so a few extra bytes past the last segment are silently ignored —
 * the file is still a perfectly valid, executable ELF.
 *
 * This is what lets the launcher and the REST API show "STAR CATCHER"
 * instead of guessing it back out of the 8.3-truncated filename
 * STARCATC.ELF. Files without the trailer (or with a bad magic) just
 * fall back to a filename-derived title — nothing requires it.
 */

#define ARCADE_META_MAGIC "ARCM"
#define ARCADE_META_SIZE  32
#define ARCADE_META_TITLE_LEN 28   /* NUL-padded, ARCADE_META_SIZE - 4 */

typedef struct {
    char magic[4];
    char title[ARCADE_META_TITLE_LEN];
} arcade_meta_t;

#endif /* GAMEMETA_H */
