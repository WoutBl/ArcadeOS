/*
 * ArcadeOS – no-code game engine implementation (see arcade_builder.h)
 */

#include "arcade_builder.h"
#include "../libc/string.h"
#include "../libc/syscall.h"

/* ──────── Small helpers ──────── */

static uint32_t shade(uint32_t c, int delta) {
    int r = (int)((c >> 16) & 0xFF) + delta; if (r < 0) r = 0; if (r > 255) r = 255;
    int g = (int)((c >> 8)  & 0xFF) + delta; if (g < 0) g = 0; if (g > 255) g = 255;
    int b = (int)( c        & 0xFF) + delta; if (b < 0) b = 0; if (b > 255) b = 255;
    return rgb((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

/* A repeating striped band — no sprite needed, still a real parallax
 * scroll once frame-indexed by scroll_speed. */
static void draw_bg_layer(surface_t* s, int screen_w, const builder_bg_layer_t* bg,
                          unsigned frame) {
    const int pitch = 48;
    int phase = (int)(((long)bg->scroll_speed * (long)frame) % pitch);
    if (phase < 0) phase += pitch;
    uint32_t c1 = bg->color;
    uint32_t c2 = shade(bg->color, -24);
    for (int x = -phase; x < screen_w; x += pitch) {
        surf_fill_rect(s, x, bg->band_y, pitch / 2, bg->band_h, c1);
        surf_fill_rect(s, x + pitch / 2, bg->band_y, pitch - pitch / 2, bg->band_h, c2);
    }
}

static int find_first_active_player(const builder_entity_t* entities,
                                     const entity_t* ents, int n) {
    for (int i = 0; i < n; i++)
        if (entities[i].role == ROLE_PLAYER && ents[i].active) return i;
    return -1;
}

/* (Re)initialize the live entity array from a level's data — used both
 * at game start and whenever a level is cleared and the next one's
 * world is swapped in. Per-entity transient state (patrol direction,
 * hit i-frames) resets with it; score/lives live above this and don't. */
static void init_level(const builder_level_t* lvl, entity_t* ents,
                       int* patrol_toward2, int* hit_cooldown,
                       int two_player, int* out_n) {
    int n = lvl->num_entities;
    if (n > BUILDER_MAX_ENTITIES) n = BUILDER_MAX_ENTITIES;
    for (int i = 0; i < n; i++) {
        const builder_entity_t* be = &lvl->entities[i];
        entity_t* e = &ents[i];
        e->x = FX(be->x); e->y = FX(be->y);
        e->vx = FX(be->vx); e->vy = FX(be->vy);
        e->w = be->w ? be->w : be->sprite->w;
        e->h = be->h ? be->h : be->sprite->h;
        e->kind = (int)be->role;
        e->sprite = be->sprite;
        e->active = 1;
        if (be->role == ROLE_PLAYER && be->player_index == 1 && !two_player)
            e->active = 0;
        patrol_toward2[i] = 0;
        hit_cooldown[i] = 0;
    }
    *out_n = n;
}

/* Move fx toward target by up to `speed` pixels/frame on each axis
 * independently (no sqrt — deterministic, fixed-point-friendly, and
 * matches the "no floats" constraint every other game already lives
 * under). Diagonal chases are a bit faster than axis-aligned ones;
 * fine for the simple homing/patrol behaviors this engine offers. */
static void steer_toward(entity_t* e, fx_t tx, fx_t ty, int speed) {
    fx_t sp = FX(speed);
    e->vx = (tx > e->x) ? sp : (tx < e->x) ? -sp : 0;
    e->vy = (ty > e->y) ? sp : (ty < e->y) ? -sp : 0;
}

/* ──────── High score (one save slot per deployed game, keyed by name) ──────── */

#define BUILDER_SAVE_MAGIC 0xB01DE500u
typedef struct { uint32_t magic; int high; } builder_save_t;

static int load_high(const char* name) {
    builder_save_t sv;
    if (arcade_load(name, 0, &sv, sizeof(sv)) == (int)sizeof(sv) &&
        sv.magic == BUILDER_SAVE_MAGIC)
        return sv.high;
    return 0;
}

static void save_high(const char* name, int high) {
    builder_save_t sv;
    sv.magic = BUILDER_SAVE_MAGIC;
    sv.high = high;
    arcade_save(name, 0, &sv, sizeof(sv));
}

/* ──────── End screen ──────── */

static void draw_end_screen(arcade_t* a, const char* title, int won,
                            int score, int high) {
    surface_t* s = &a->screen;
    const char* headline = won ? "YOU WIN" : "GAME OVER";
    surf_fill_rect(s, 0, a->h / 2 - 60, a->w, 120, rgb(10, 12, 30));
    surf_draw_text(s, a->w / 2 - (int)strlen(headline) * 12, a->h / 2 - 44,
                   headline, won ? rgb(120, 255, 160) : rgb(255, 110, 110),
                   SURF_TRANSPARENT, 3);
    surf_draw_text(s, a->w / 2 - (int)strlen(title) * 8, a->h / 2 - 4, title,
                   rgb(200, 205, 230), SURF_TRANSPARENT, 1);

    char line[32] = "SCORE ";
    num_to_str(line + 6, score, 1);
    int ln = (int)strlen(line);
    strcpy(line + ln, "  HI ");
    num_to_str(line + ln + 5, high, 1);
    surf_draw_text(s, a->w / 2 - (int)strlen(line) * 4, a->h / 2 + 20, line,
                   rgb(255, 220, 80), SURF_TRANSPARENT, 1);

    surf_draw_text(s, a->w / 2 - 88, a->h / 2 + 44, "PRESS ANY BUTTON",
                   rgb(140, 150, 190), SURF_TRANSPARENT, 1);
}

/* Shown for LEVEL_TRANSITION_FRAMES after an FX_WIN hit on any level
 * but the last. next_1idx is the upcoming level's 1-indexed number. */
#define LEVEL_TRANSITION_FRAMES 90

static void draw_level_clear_screen(arcade_t* a, int next_1idx, int total) {
    surface_t* s = &a->screen;
    const char* headline = "LEVEL CLEARED";
    surf_fill_rect(s, 0, a->h / 2 - 40, a->w, 80, rgb(10, 12, 30));
    surf_draw_text(s, a->w / 2 - (int)strlen(headline) * 8, a->h / 2 - 24,
                   headline, rgb(120, 255, 160), SURF_TRANSPARENT, 2);

    char line[24] = "LEVEL ";
    num_to_str(line + 6, next_1idx, 1);
    int ln = (int)strlen(line);
    strcpy(line + ln, " OF ");
    num_to_str(line + ln + 4, total, 1);
    surf_draw_text(s, a->w / 2 - (int)strlen(line) * 4, a->h / 2 + 8, line,
                   rgb(200, 205, 230), SURF_TRANSPARENT, 1);
}

/* ──────── The engine ──────── */

void arcade_builder_run(const builder_game_t* cfg) {
    arcade_t a;
    if (arcade_init(&a) != 0) return;

    int two_player = (cfg->session == BUILDER_SESSION_2P);
    if (cfg->session == BUILDER_SESSION_CHOOSE) {
        int mode = arcade_choose_players(&a, cfg->title, 0);
        if (mode == ARCADE_MODE_QUIT) return;
        two_player = (mode == ARCADE_MODE_2P);
    }

    int cur_level = 0;
    const builder_level_t* lvl = &cfg->levels[cur_level];

    entity_t ents[BUILDER_MAX_ENTITIES];
    int patrol_toward2[BUILDER_MAX_ENTITIES];
    /* Brief i-frames per entity after it hits a player: without this,
     * standing inside a non-removed enemy (e.g. a homing chaser) fires
     * its effect every single frame — a full life bar gone in an
     * instant. ~0.5s at the fixed 60 FPS timestep. Reset with the rest
     * of an entity's state whenever a level is (re)initialized. */
    int hit_cooldown[BUILDER_MAX_ENTITIES];
    int n;
    init_level(lvl, ents, patrol_toward2, hit_cooldown, two_player, &n);

    int high = load_high(cfg->name);
    int score = 0, lives = cfg->lives;
    unsigned start_tick = ticks();
    int ended = 0, won = 0, transition = 0;

    while (arcade_frame(&a)) {
        a.score = score;

        if (transition > 0) {
            transition--;
            if (transition == 0) {
                cur_level++;
                lvl = &cfg->levels[cur_level];
                init_level(lvl, ents, patrol_toward2, hit_cooldown, two_player, &n);
            }
        } else if (!ended) {
            /* ── Behaviors ── */
            for (int i = 0; i < n; i++) {
                const builder_entity_t* be = &lvl->entities[i];
                entity_t* e = &ents[i];
                if (!e->active) continue;

                switch (be->behavior) {
                case BEH_PLAYER_MOVE: {
                    uint16_t held = (be->player_index == 1) ? a.held2 : a.held;
                    fx_t sp = FX(be->speed);
                    e->vx = e->vy = 0;
                    if (held & PAD_BTN_LEFT)  e->vx = -sp;
                    if (held & PAD_BTN_RIGHT) e->vx =  sp;
                    if (held & PAD_BTN_UP)    e->vy = -sp;
                    if (held & PAD_BTN_DOWN)  e->vy =  sp;
                    arcade_entity_move(e);
                    if (FX_INT(e->x) < 0) e->x = 0;
                    if (FX_INT(e->x) > a.w - e->w) e->x = FX(a.w - e->w);
                    if (FX_INT(e->y) < 0) e->y = 0;
                    if (FX_INT(e->y) > a.h - e->h) e->y = FX(a.h - e->h);
                    break;
                }
                case BEH_BOUNCE:
                    arcade_entity_bounce(e, a.w, a.h);
                    break;
                case BEH_HOME: {
                    int p = find_first_active_player(lvl->entities, ents, n);
                    if (p >= 0) steer_toward(e, ents[p].x, ents[p].y, be->speed);
                    arcade_entity_move(e);
                    break;
                }
                case BEH_PATROL: {
                    fx_t tx = patrol_toward2[i] ? FX(be->patrol_x2) : FX(be->x);
                    fx_t ty = patrol_toward2[i] ? FX(be->patrol_y2) : FX(be->y);
                    steer_toward(e, tx, ty, be->speed);
                    arcade_entity_move(e);
                    fx_t sp = FX(be->speed);
                    if (e->x > tx - sp && e->x < tx + sp &&
                        e->y > ty - sp && e->y < ty + sp)
                        patrol_toward2[i] = !patrol_toward2[i];
                    break;
                }
                case BEH_STATIC: default: break;
                }
            }

            /* ── Collisions: non-players vs every active player ── */
            for (int i = 0; i < n; i++) {
                const builder_entity_t* be = &lvl->entities[i];
                entity_t* e = &ents[i];
                if (hit_cooldown[i] > 0) hit_cooldown[i]--;
                if (be->role == ROLE_PLAYER || be->on_player_hit.effect == FX_NONE) continue;
                if (!e->active || hit_cooldown[i] > 0) continue;

                for (int j = 0; j < n; j++) {
                    if (lvl->entities[j].role != ROLE_PLAYER || !ents[j].active) continue;
                    if (!arcade_entity_overlap(e, &ents[j])) continue;

                    /* A silent no-op off a USB pad — see arcade_rumble(). */
                    int hit_player = lvl->entities[j].player_index;
                    switch (be->on_player_hit.effect) {
                    case FX_SCORE:
                        score += be->on_player_hit.amount; sfx_score();
                        arcade_rumble(hit_player, 90, 40);
                        break;
                    case FX_LOSE_LIFE:
                        lives--; sfx_lose();
                        arcade_rumble(hit_player, 200, 150);
                        break;
                    case FX_WIN:
                        if (cur_level + 1 < cfg->num_levels) { transition = LEVEL_TRANSITION_FRAMES; sfx_score(); }
                        else                                  { ended = 1; won = 1; }
                        arcade_rumble(hit_player, 150, 200);
                        break;
                    case FX_GAMEOVER:
                        ended = 1; won = 0;
                        arcade_rumble(hit_player, 255, 400);
                        break;
                    default: break;
                    }
                    if (be->on_player_hit.remove_self) e->active = 0;
                    else                                hit_cooldown[i] = 30;
                    break;
                }
            }

            if (!ended && cfg->win_score > 0 && score >= cfg->win_score) { ended = 1; won = 1; }
            if (!ended && cfg->lives > 0 && lives <= 0) { ended = 1; won = 0; }
            if (!ended && cfg->time_limit_s > 0 &&
                (ticks() - start_tick) >= (unsigned)cfg->time_limit_s * 1000u)
                { ended = 1; won = 0; }

            if (ended && score > high) { high = score; save_high(cfg->name, high); }
        } else if (a.pressed || a.pressed2) {
            exit(0);
        }

        /* ── Draw ── */
        if (transition > 0) {
            surf_clear(&a.screen, rgb(6, 8, 20));
            draw_level_clear_screen(&a, cur_level + 2, cfg->num_levels);
            continue;
        }

        surf_clear(&a.screen, rgb(6, 8, 20));
        for (int i = 0; i < lvl->num_bg_layers && i < BUILDER_MAX_BG_LAYERS; i++)
            draw_bg_layer(&a.screen, a.w, &lvl->bg_layers[i], a.frame);
        for (int i = 0; i < lvl->num_tile_layers && i < BUILDER_MAX_TILE_LAYERS; i++)
            arcade_draw_tilemap(&a.screen, &lvl->tile_layers[i].map, lvl->tile_layers[i].colors);
        for (int i = 0; i < n; i++)
            arcade_entity_draw(&a.screen, &ents[i], 1);

        /* HUD */
        {
            char line[24] = "SCORE ";
            num_to_str(line + 6, score, 1);
            surf_draw_text(&a.screen, 8, 8, line, rgb(230, 230, 240), SURF_TRANSPARENT, 1);
        }
        if (cfg->lives > 0) {
            char line[8] = "x";
            num_to_str(line + 1, lives, 1);
            surf_draw_text(&a.screen, a.w - 8 - (int)strlen(line) * 8, 8, line,
                           rgb(255, 110, 110), SURF_TRANSPARENT, 1);
        }
        if (cfg->num_levels > 1) {
            char line[16] = "LVL ";
            num_to_str(line + 4, cur_level + 1, 1);
            surf_draw_text(&a.screen, a.w / 2 - (int)strlen(line) * 4, 8, line,
                           rgb(150, 160, 200), SURF_TRANSPARENT, 1);
        }

        if (ended) draw_end_screen(&a, cfg->title, won, score, high);
    }
}
