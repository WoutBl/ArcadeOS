/*
 * ArcadeOS – PONG (Ring 3 game, built on the ArcadeOS SDK)
 *
 * The reference title proving the full console loop:
 * FAT32 load → ELF exec → Ring 3 → pad input syscall → gfx present syscall.
 *
 * Controls: P1 (pad 0) = arrows/left stick, P2 (pad 1) = W/S keys.
 *           Opens with the SDK player-select screen (1P vs CPU / 2P).
 *           START = pause, SELECT or B = quit to launcher
 */

#include "../sdk/arcade.h"
#include "../libc/syscall.h"
#include "../libc/string.h"

/* Persistent high score: most points player 1 racked up in a session */
#define SAVE_MAGIC 0xA2CADE03u
typedef struct { unsigned int magic; int high; } save_t;

/* ──────── Netplay (host-authoritative) ────────
 * Client → host every frame: its pad. Host → client every frame: the
 * whole (tiny) game state. On a LAN that is one frame of latency. */
#define PONG_PORT 7777
#define PI_MAGIC  0x50494E31u   /* 'PIN1' client input */
#define PS_MAGIC  0x50535431u   /* 'PST1' host state */

typedef struct {
    unsigned int   magic;
    short          ly;          /* Client analog Y */
    unsigned short held;        /* Client buttons */
    unsigned char  bye, _pad;
} np_input_t;

typedef struct {
    unsigned int  magic;
    short         ball_x, ball_y;
    short         left_y, right_y;
    unsigned char score_l, score_r, paused, bye;
} np_state_t;

/* "Opponent left" card, then back to the launcher */
static void np_farewell(arcade_t* a, const char* why) {
    for (int i = 0; i < 120 && arcade_frame(a); i++) {
        surf_clear(&a->screen, rgb(8, 10, 30));
        surf_draw_text(&a->screen, a->w / 2 - (int)(strlen(why) * 12), a->h / 2 - 12,
                       why, rgb(255, 220, 80), SURF_TRANSPARENT, 3);
    }
    exit(0);
}

static void draw_score(surface_t* s, int x, int score, uint32_t color) {
    char buf[4];
    if (score > 99) score = 99;
    buf[0] = (char)('0' + score / 10);
    buf[1] = (char)('0' + score % 10);
    buf[2] = '\0';
    surf_draw_text(s, x, 24, buf, color, SURF_TRANSPARENT, 4);
}

int main(void) {
    arcade_t a;
    if (arcade_init(&a) != 0) {
        write(1, "pong: no usable framebuffer\n", 28);
        exit(1);
    }

    const int W = a.w, H = a.h;
    const int paddle_w = 12, paddle_h = 80;
    const int ball_size = 10;
    const int paddle_speed = 6;

    int left_y  = H / 2 - paddle_h / 2;
    int right_y = H / 2 - paddle_h / 2;
    int score_l = 0, score_r = 0;
    int paused = 0;

    /* Standard opener: pick the mode on the SDK player-select screen */
    int mode = arcade_choose_players(&a, "PONG", ARCADE_CHOOSE_NET);
    if (mode == ARCADE_MODE_QUIT) exit(0);
    int two_player = (mode != ARCADE_MODE_1P);

    /* Online: find the opponent on the LAN first */
    int      net_role = 0;      /* 0 local, 1 host, 2 client */
    unsigned peer_ip  = 0;
    if (mode == ARCADE_MODE_NET_HOST) {
        if (arcade_net_host_wait(&a, PONG_PORT, "PONG", &peer_ip) != 0) exit(0);
        net_role = 1;
    } else if (mode == ARCADE_MODE_NET_JOIN) {
        if (arcade_net_join_lan(&a, PONG_PORT, "PONG", &peer_ip) != 0) exit(0);
        net_role = 2;
    }
    np_input_t net_in    = {0};     /* Host: the client's latest pad */
    int        net_stale = 0;      /* Frames since the peer spoke */

    save_t sv;
    int high = 0, high_dirty = 0;
    if (arcade_load("PONG", 0, &sv, sizeof(sv)) == (int)sizeof(sv) &&
        sv.magic == SAVE_MAGIC)
        high = sv.high;

    entity_t ball = { 0 };
    ball.active = 1;
    ball.w = ball.h = ball_size;
    ball.x  = FX(W / 2);
    ball.y  = FX(H / 2);
    ball.vx = FX(4);
    ball.vy = (fx_t)(arcade_rand() % 512) - 256 + FX(2);

    while (arcade_frame(&a)) {
        a.score = score_l;
        if (a.pressed & (PAD_BTN_SELECT | PAD_BTN_B)) {
            if (net_role == 1) {
                np_state_t st = {0};
                st.magic = PS_MAGIC; st.bye = 1;
                arcade_net_send(peer_ip, PONG_PORT, &st, sizeof(st));
            } else if (net_role == 2) {
                np_input_t in = {0};
                in.magic = PI_MAGIC; in.bye = 1;
                arcade_net_send(peer_ip, PONG_PORT, &in, sizeof(in));
            }
            if (high_dirty) {
                sv.magic = SAVE_MAGIC;
                sv.high  = high;
                arcade_save("PONG", 0, &sv, sizeof(sv));
            }
            exit(0);   /* Back to the launcher */
        }
        if (a.pressed & PAD_BTN_START)
            paused = !paused;

        /* ──────── Netplay I/O ──────── */
        if (net_role == 1) {
            /* Drain the client's inputs, keep the newest */
            np_input_t in;
            int got = 0;
            while (arcade_net_recv(&in, sizeof(in), 0, 0) >= (int)sizeof(in)) {
                if (in.magic != PI_MAGIC) continue;
                if (in.bye) np_farewell(&a, "OPPONENT LEFT");
                net_in = in;
                got = 1;
            }
            net_stale = got ? 0 : net_stale + 1;
            if (net_stale > 300) np_farewell(&a, "CONNECTION LOST");
        } else if (net_role == 2) {
            /* Send our pad, apply the host's newest state */
            np_input_t in = {0};
            in.magic = PI_MAGIC;
            in.ly    = a.pad.ly;
            in.held  = a.held;
            arcade_net_send(peer_ip, PONG_PORT, &in, sizeof(in));

            np_state_t st;
            int got = 0;
            while (arcade_net_recv(&st, sizeof(st), 0, 0) >= (int)sizeof(st)) {
                if (st.magic != PS_MAGIC) continue;
                if (st.bye) np_farewell(&a, "HOST LEFT");
                ball.x  = FX(st.ball_x);
                ball.y  = FX(st.ball_y);
                left_y  = st.left_y;
                right_y = st.right_y;
                if (st.score_r > score_r) sfx_score();
                if (st.score_l > score_l) sound(180, 250);
                score_l = st.score_l;
                score_r = st.score_r;
                paused  = st.paused;
                got = 1;
            }
            net_stale = got ? 0 : net_stale + 1;
            if (net_stale > 300) np_farewell(&a, "CONNECTION LOST");
            a.score = score_r;              /* The client IS the right side */
        }

        if (!paused && net_role != 2) {
            /* Player paddle: D-pad or analog stick */
            if (a.held & PAD_BTN_UP)    left_y -= paddle_speed;
            if (a.held & PAD_BTN_DOWN)  left_y += paddle_speed;
            if (a.pad.ly < -8000) left_y -= paddle_speed;
            if (a.pad.ly >  8000) left_y += paddle_speed;
            if (left_y < 0) left_y = 0;
            if (left_y > H - paddle_h) left_y = H - paddle_h;

            if (net_role == 1) {
                /* Online: the challenger's pad drives the right paddle */
                if (net_in.held & PAD_BTN_UP)   right_y -= paddle_speed;
                if (net_in.held & PAD_BTN_DOWN) right_y += paddle_speed;
                if (net_in.ly < -8000) right_y -= paddle_speed;
                if (net_in.ly >  8000) right_y += paddle_speed;
            } else if (two_player) {
                /* Player 2 paddle: pad 1 (W/S on the keyboard) */
                if (a.held2 & PAD_BTN_UP)    right_y -= paddle_speed;
                if (a.held2 & PAD_BTN_DOWN)  right_y += paddle_speed;
                if (a.pad2.ly < -8000) right_y -= paddle_speed;
                if (a.pad2.ly >  8000) right_y += paddle_speed;
            } else {
                /* AI paddle: track the ball with a speed cap */
                int ball_cy = FX_INT(ball.y) + ball_size / 2;
                int ai_cy   = right_y + paddle_h / 2;
                if (ai_cy < ball_cy - 8) right_y += paddle_speed - 2;
                if (ai_cy > ball_cy + 8) right_y -= paddle_speed - 2;
            }
            if (right_y < 0) right_y = 0;
            if (right_y > H - paddle_h) right_y = H - paddle_h;

            /* Ball physics */
            arcade_entity_move(&ball);

            int bx = FX_INT(ball.x);
            int by = FX_INT(ball.y);

            /* Top/bottom walls */
            if (by <= 0)              { ball.y = 0;                  ball.vy = -ball.vy; }
            if (by >= H - ball_size)  { ball.y = FX(H - ball_size);  ball.vy = -ball.vy; }

            /* Left paddle */
            if (bx <= 24 + paddle_w && bx >= 24 &&
                arcade_aabb(bx, by, ball_size, ball_size, 24, left_y, paddle_w, paddle_h) &&
                ball.vx < 0) {
                ball.vx = -ball.vx + 32;    /* Speed up slightly each hit */
                /* Add english based on where the paddle was struck */
                ball.vy += ((by + ball_size / 2) - (left_y + paddle_h / 2)) << 4;
                sfx_hit();
            }

            /* Right paddle */
            if (bx + ball_size >= W - 24 - paddle_w && bx + ball_size <= W - 24 &&
                arcade_aabb(bx, by, ball_size, ball_size,
                            W - 24 - paddle_w, right_y, paddle_w, paddle_h) &&
                ball.vx > 0) {
                ball.vx = -(ball.vx + 32);
                ball.vy += ((by + ball_size / 2) - (right_y + paddle_h / 2)) << 4;
                sfx_hit();
            }

            /* Clamp vertical speed */
            if (ball.vy >  FX(6)) ball.vy =  FX(6);
            if (ball.vy < -FX(6)) ball.vy = -FX(6);

            /* Scoring */
            if (bx < -ball_size)  { score_r++; sound(180, 250); ball.x = FX(W/2); ball.y = FX(H/2); ball.vx =  FX(4); ball.vy = (fx_t)(arcade_rand() % 512) - 256; }
            if (bx > W)           { score_l++; if (score_l > high) { high = score_l; high_dirty = 1; } sound(700, 150); ball.x = FX(W/2); ball.y = FX(H/2); ball.vx = -FX(4); ball.vy = (fx_t)(arcade_rand() % 512) - 256; }
        }

        if (net_role == 1) {
            np_state_t st;
            st.magic   = PS_MAGIC;
            st.ball_x  = (short)FX_INT(ball.x);
            st.ball_y  = (short)FX_INT(ball.y);
            st.left_y  = (short)left_y;
            st.right_y = (short)right_y;
            st.score_l = (unsigned char)(score_l > 255 ? 255 : score_l);
            st.score_r = (unsigned char)(score_r > 255 ? 255 : score_r);
            st.paused  = (unsigned char)paused;
            st.bye     = 0;
            arcade_net_send(peer_ip, PONG_PORT, &st, sizeof(st));
        }

        /* ──────── Render ──────── */
        surf_clear(&a.screen, rgb(6, 6, 18));

        /* Center line */
        for (int y = 0; y < H; y += 24)
            surf_fill_rect(&a.screen, W / 2 - 2, y, 4, 12, rgb(60, 70, 120));

        draw_score(&a.screen, W / 2 - 96, score_l, rgb(120, 200, 255));
        draw_score(&a.screen, W / 2 + 40, score_r, rgb(255, 160, 120));

        surf_fill_rect(&a.screen, 24, left_y, paddle_w, paddle_h, rgb(120, 200, 255));
        surf_fill_rect(&a.screen, W - 24 - paddle_w, right_y, paddle_w, paddle_h, rgb(255, 160, 120));
        surf_fill_rect(&a.screen, FX_INT(ball.x), FX_INT(ball.y), ball_size, ball_size, rgb(255, 255, 255));

        if (paused)
            surf_draw_text(&a.screen, W / 2 - 96, H / 2 - 16, "PAUSED",
                           rgb(255, 220, 80), SURF_TRANSPARENT, 4);

        {
            char hb[12] = "HI ";
            int v = high, n = 3;
            if (v <= 0) hb[n++] = '0';
            char tmp[8]; int t = 0;
            while (v > 0 && t < 7) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
            while (t > 0) hb[n++] = tmp[--t];
            hb[n] = '\0';
            surf_draw_text(&a.screen, 16, 16, hb, rgb(255, 220, 80), SURF_TRANSPARENT, 1);
        }
        surf_draw_text(&a.screen, W / 2 - 32, 76,
                       net_role == 1 ? "NET HOST" :
                       net_role == 2 ? "NET AWAY" :
                       two_player    ? "2P MODE"  : "VS CPU",
                       net_role      ? rgb(255, 180, 255) :
                       two_player    ? rgb(120, 255, 160) : rgb(90, 100, 150),
                       SURF_TRANSPARENT, 1);
        surf_draw_text(&a.screen, 16, H - 16,
                       "SELECT/B: QUIT  START: PAUSE  P2: W/S",
                       rgb(80, 90, 140), SURF_TRANSPARENT, 1);
    }

    return 0;
}
