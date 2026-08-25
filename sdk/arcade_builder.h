#ifndef ARCADE_BUILDER_H
#define ARCADE_BUILDER_H

#include "arcade.h"

/*
 * ArcadeOS – no-code game engine
 *
 * A generic game loop driven entirely by data. The visual builder
 * (tools/arcade-dev/game-builder.html) assembles a builder_game_t; its
 * local bridge (tools/arcade-dev/codegen.py) emits nothing but struct
 * literals of these types plus a 3-line main() that calls
 * arcade_builder_run(&cfg) — the game itself is never hand-written C.
 *
 * Every behavior/effect here is built entirely from existing SDK
 * primitives (arcade_entity_move/bounce/overlap, arcade_draw_tilemap,
 * arcade_save/load, arcade_choose_players, sfx_*) — this module adds
 * no new kernel surface, it just composes what libarcade already has.
 */

#define BUILDER_MAX_ENTITIES    32
#define BUILDER_MAX_BG_LAYERS    3
#define BUILDER_MAX_TILE_LAYERS  3
#define BUILDER_MAX_LEVELS      16

typedef enum {
    BEH_STATIC = 0,     /* Never moves (pickups, obstacles) */
    BEH_PLAYER_MOVE,    /* 8-way pad input, screen-clamped */
    BEH_BOUNCE,         /* Linear motion, bounces off screen edges */
    BEH_HOME,           /* Steers toward the nearest active player entity */
    BEH_PATROL,         /* Ping-pongs between (x,y) and (patrol_x2,patrol_y2) */
} behavior_kind_t;

typedef enum {
    FX_NONE = 0,        /* No collision rule (this entity never reacts) */
    FX_SCORE,
    FX_LOSE_LIFE,
    FX_WIN,             /* Clears the level: advances to the next one, or
                         * shows the win screen if this is the last level */
    FX_GAMEOVER,
} collision_effect_t;

typedef enum {
    ROLE_PLAYER = 0,
    ROLE_ENEMY,
    ROLE_PICKUP,
} entity_role_t;

/* Evaluated when this entity overlaps an active player entity */
typedef struct {
    collision_effect_t effect;
    int                amount;       /* FX_SCORE: points awarded */
    int                remove_self;  /* 1 = deactivate this entity on hit */
} collision_rule_t;

typedef struct {
    entity_role_t   role;
    int             player_index;    /* 0 or 1; only meaningful for ROLE_PLAYER */
    const sprite_t* sprite;          /* required */
    int             x, y;            /* starting position, pixels */
    int             w, h;            /* collision box; 0,0 = use sprite size */
    int             vx, vy;          /* starting velocity, pixels/frame (BEH_BOUNCE) */
    int             speed;           /* pixels/frame (BEH_PLAYER_MOVE/HOME/PATROL) */
    behavior_kind_t behavior;
    int             patrol_x2, patrol_y2;   /* second waypoint (BEH_PATROL) */
    collision_rule_t on_player_hit;
} builder_entity_t;

/* A decorative, horizontally-scrolling striped band — no sprite needed,
 * just a color and a scroll speed; still gives a real parallax effect. */
typedef struct {
    uint32_t color;
    int      scroll_speed;   /* pixels/frame; sign sets direction */
    int      band_y, band_h; /* vertical extent on screen */
} builder_bg_layer_t;

typedef struct {
    tilemap_t       map;
    const uint32_t* colors;
    uint32_t        solid_mask;   /* only consulted on the LAST tile layer */
} builder_tile_layer_t;

typedef enum {
    BUILDER_SESSION_1P = 0,
    BUILDER_SESSION_2P,
    BUILDER_SESSION_CHOOSE,   /* Shows arcade_choose_players() at start */
} builder_session_t;

/* One stage: its own world (layers + entities). Clearing a level (an
 * FX_WIN hit) swaps in the next level's world — score, lives, and the
 * session carry over, only the world resets. */
typedef struct {
    const builder_bg_layer_t*   bg_layers;   int num_bg_layers;
    const builder_tile_layer_t* tile_layers; int num_tile_layers;
    const builder_entity_t*     entities;    int num_entities;
} builder_level_t;

typedef struct {
    const char* name;          /* <=7 chars: save-slot key + filename fallback */
    const char* title;         /* shown on the win/game-over screen */
    builder_session_t session;

    int lives;                 /* 0 = no lives (endless / score-attack) */
    int win_score;              /* 0 = no win condition */
    int time_limit_s;           /* 0 = no timer */

    const builder_level_t* levels; int num_levels;   /* played in order */
} builder_game_t;

/* Runs the whole game (never returns; the game exits via exit() like
 * any other ArcadeOS app once the player backs out of the end screen). */
void arcade_builder_run(const builder_game_t* cfg);

#endif /* ARCADE_BUILDER_H */
