#ifndef ARCADE_H
#define ARCADE_H

/*
 * ArcadeOS Game SDK (libarcade)
 *
 * The framework layer for writing ArcadeOS games. Sits on top of the
 * syscall libc (libc/console.h) and adds everything the built-in games
 * had to hand-roll: a fixed-timestep game loop, input edge detection,
 * fixed-point math, sprites, entities, AABB collision, save slots,
 * canned sound effects, and a PRNG.
 *
 * Quick start (see sdk/README.md for the full template):
 *
 *     #include "arcade.h"
 *     int main(void) {
 *         arcade_t a;
 *         if (arcade_init(&a) != 0) return 1;
 *         while (arcade_frame(&a)) {
 *             if (a.pressed & PAD_BTN_START) { ... }
 *             surf_clear(&a.screen, rgb(8, 10, 30));
 *             ...draw...
 *         }
 *         return 0;
 *     }
 */

#include "../libc/console.h"

/* ──────── Fixed-point math (24.8, like the built-in games) ──────── */

typedef int32_t fx_t;
#define FX(v)        ((fx_t)((v) << 8))       /* int → fixed */
#define FX_INT(v)    ((int)((v) >> 8))        /* fixed → int (floor) */
#define FX_MUL(a, b) ((fx_t)(((int64_t)(a) * (b)) >> 8))

/* ──────── Sprites (0x00RRGGBB pixels, SURF_TRANSPARENT = skip) ──────── */

typedef struct {
    const uint32_t* pixels;
    int w, h;
} sprite_t;

void arcade_draw_sprite(surface_t* s, const sprite_t* spr, int x, int y, int scale);

/* ──────── Entities: position/velocity in fixed-point ──────── */

typedef struct {
    fx_t x, y;           /* Position (fixed-point pixels) */
    fx_t vx, vy;         /* Velocity per frame */
    int  w, h;           /* Size in pixels (collision box) */
    int  active;
    int  kind;           /* Game-defined tag */
    const sprite_t* sprite;   /* Optional (NULL = game draws it itself) */
} entity_t;

/* x += vx, y += vy */
void arcade_entity_move(entity_t* e);

/* Move and bounce off the screen edges (inverts velocity). Returns a
 * bitmask of edges hit: 1=left 2=right 4=top 8=bottom. */
int arcade_entity_bounce(entity_t* e, int screen_w, int screen_h);

/* Axis-aligned overlap test between two active entities */
int arcade_entity_overlap(const entity_t* a, const entity_t* b);

/* Raw AABB test (pixel coordinates) */
int arcade_aabb(int x1, int y1, int w1, int h1,
                int x2, int y2, int w2, int h2);

/* Draw e->sprite at the entity position (no-op when sprite is NULL) */
void arcade_entity_draw(surface_t* s, const entity_t* e, int scale);

/* ──────── Save slots ──────── */

/* Whole-file save/load in slot 0-9. `game` is at most 7 characters
 * (8.3 names: the slot digit is appended, e.g. "SNAKE" slot 0 →
 * "SNAKE0.SAV"). Returns 0 / bytes read, -1 on failure. */
int arcade_save(const char* game, int slot, const void* buf, int len);
int arcade_load(const char* game, int slot, void* buf, int maxlen);

/* ──────── Sound effects ──────── */

/* Canned SFX so games sound consistent without tuning frequencies.
 * These run on mixer voices, so overlapping effects blend instead of
 * cutting each other off (voice 0: legacy sound(); voice 1: canned
 * tones; voice 2: PCM effects; voice 3: reserved for the system). */
static inline void sfx_tone_v(int voice, int freq, int ms, int vol) {
    sound_req_t rq = {0};
    rq.voice = (uint32_t)voice; rq.op = SOUND_OP_SQUARE;
    rq.vol = (uint32_t)vol; rq.freq_hz = (uint32_t)freq;
    rq.dur_ms = (uint32_t)ms;
    sound_ex(&rq);
}

/* Play a 16-bit mono PCM clip on a voice (copied by the kernel). */
static inline int sfx_pcm(int voice, const int16_t* data, int count,
                          int rate, int vol) {
    sound_req_t rq = {0};
    rq.voice = (uint32_t)voice; rq.op = SOUND_OP_PCM;
    rq.vol = (uint32_t)vol; rq.sample_rate = (uint32_t)rate;
    rq.sample_count = (uint32_t)count;
    rq.sample_ptr = (uint64_t)(uintptr_t)data;
    return sound_ex(&rq);
}

static inline void sfx_move(void)     { sfx_tone_v(1, 600, 30, 255);  }
static inline void sfx_select(void)   { sfx_tone_v(1, 900, 80, 255);  }
static inline void sfx_hit(void)      { sfx_tone_v(1, 440, 40, 255);  }
static inline void sfx_score(void)    { sfx_tone_v(1, 880, 60, 255);  }
static inline void sfx_lose(void)     { sfx_tone_v(1, 160, 300, 255); }
static inline void sfx_gameover(void) { sfx_tone_v(1, 150, 400, 255); }

/* Noise-burst explosion, generated once and played as PCM (voice 2).
 * Uses its own PRNG so the game's arcade_rand() sequence — and with it
 * any determinism the game relies on — is untouched. */
void sfx_explosion(void);


/* ──────── Netplay (UDP) ────────
 *
 * A single datagram socket per game. IPs are host-order uint32:
 * ARCADE_IP(10,0,2,2) is QEMU's host gateway. Datagrams cap at
 * NET_MSG_MAX (512) bytes — send your input/state snapshot, not the
 * framebuffer. arcade_net_send returns -2 while ARP resolves the
 * peer; just retry on the next frame.
 */

#define ARCADE_IP(a, b, c, d) \
    (((unsigned)(a) << 24) | ((unsigned)(b) << 16) | \
     ((unsigned)(c) << 8) | (unsigned)(d))

static inline unsigned arcade_net_local_ip(void) {
    net_req_t rq = {0};
    rq.op = NET_OP_INFO;
    return (unsigned)net_op(&rq);
}

static inline int arcade_net_bind(int port) {
    net_req_t rq = {0};
    rq.op = NET_OP_BIND; rq.port = (uint32_t)port;
    return net_op(&rq);
}

static inline int arcade_net_send(unsigned ip, int port,
                                  const void* buf, int len) {
    net_req_t rq = {0};
    rq.op = NET_OP_SEND; rq.ip = ip; rq.port = (uint32_t)port;
    rq.len = (uint32_t)len; rq.buf = (uint64_t)(uintptr_t)buf;
    return net_op(&rq);
}

/* Returns bytes received (or -1 when nothing is queued); fills
 * src_ip/src_port when non-NULL. */
static inline int arcade_net_recv(void* buf, int maxlen,
                                  unsigned* src_ip, int* src_port) {
    net_req_t rq = {0};
    rq.op = NET_OP_RECV; rq.len = (uint32_t)maxlen;
    rq.buf = (uint64_t)(uintptr_t)buf;
    int n = net_op(&rq);
    if (n >= 0) {
        if (src_ip)   *src_ip   = rq.ip;
        if (src_port) *src_port = (int)rq.port;
    }
    return n;
}

/* ──────── Session (who is playing) ────────
 *
 * The launcher declares the active players; games read them back for
 * name tags. Returns the player count (1 or 2). Buffers must hold
 * SESSION_NAME_LEN bytes.
 */
static inline int arcade_session(char* p1, char* p2) {
    session_req_t rq = {0};
    rq.op = SESSION_OP_GET;
    if (session_op(&rq) != 0) {
        if (p1) p1[0] = '\0';
        if (p2) p2[0] = '\0';
        return 1;
    }
    for (int i = 0; i < SESSION_NAME_LEN; i++) {
        if (p1) p1[i] = rq.p1[i];
        if (p2) p2[i] = rq.p2[i];
    }
    return (int)rq.count;
}

/* ──────── PRNG (xorshift32, seeded from the clock) ──────── */

void     arcade_srand(uint32_t seed);
uint32_t arcade_rand(void);
/* Uniform integer in [lo, hi] */
int      arcade_rand_range(int lo, int hi);

/* ──────── The game context ──────── */

/* Largest mode the bootloader will negotiate (see boot/stage2.asm's
 * vbe_prefs). The static framebuffer is sized for the worst case;
 * games always draw at the REAL size in a->w / a->h. */
#define ARCADE_MAX_W 1024
#define ARCADE_MAX_H 768

typedef struct {
    surface_t    screen;     /* Draw into this every frame */
    int          w, h;       /* Screen size in pixels */

    pad_state_t  pad;        /* Raw pad 0 state this frame (player 1) */
    uint16_t     pressed;    /* Buttons that went down THIS frame */
    uint16_t     released;   /* Buttons that went up THIS frame */
    uint16_t     held;       /* Buttons currently down */

    /* Player 2 (pad 1). On the keyboard: WASD = D-pad/stick, R/T/F/G =
     * A/B/X/Y. Same edge fields as player 1. */
    pad_state_t  pad2;
    uint16_t     pressed2;
    uint16_t     released2;
    uint16_t     held2;

    unsigned int frame;      /* Frame counter */
    unsigned int frame_ms;   /* Fixed timestep (default 16 ≈ 60 FPS) */

    /* Set this to your current score each frame: the SDK reports it to
     * the kernel when it changes, so the REST API (/api/status) shows
     * the live score while you play. */
    int          score;
} arcade_t;

/* Initialize graphics + input + PRNG. Returns 0 on success. */
int arcade_init(arcade_t* a);

/* End-of-frame: present the screen, pace to the fixed timestep, then
 * poll the pad and compute the pressed/released edges for the NEXT
 * iteration. Always returns 1 (loop forever; games exit via exit()). */
int arcade_frame(arcade_t* a);

/* ──────── Player-select screen ────────
 *
 * The standard opener for 2P-capable games: shows the session
 * usernames and lets the player pick a mode with the pad. Runs its
 * own frame loop; returns when a mode is chosen (A) or the player
 * backs out (B/SELECT → ARCADE_MODE_QUIT, the game should exit(0)).
 * Pass ARCADE_CHOOSE_NET to also offer online host/join entries.
 */
#define ARCADE_MODE_QUIT     (-1)
#define ARCADE_MODE_1P        0    /* vs CPU */
#define ARCADE_MODE_2P        1    /* Local: pad 0 vs pad 1 */
#define ARCADE_MODE_NET_HOST  2    /* Online: this console hosts */
#define ARCADE_MODE_NET_JOIN  3    /* Online: join a host on the LAN */

#define ARCADE_CHOOSE_NET     0x1

int arcade_choose_players(arcade_t* a, const char* title, unsigned flags);


#endif /* ARCADE_H */
