/*
 * ArcadeOS – PONG (Ring 3 game, built on the ArcadeOS SDK)
 *
 * The reference title proving the full console loop:
 * FAT32 load → ELF exec → Ring 3 → pad input syscall → gfx present syscall.
 *
 * Controls: P1 (pad 0) = arrows/left stick, P2 (pad 1) = W/S keys.
 *           Y (V key) toggles 1P (AI opponent) / 2P mode.
 *           START = pause, SELECT or B = quit to launcher
 */

#include "../sdk/arcade.h"
#include "../libc/syscall.h"

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
    int two_player = 0;      /* Y toggles: 0 = AI opponent, 1 = pad 1 */

    entity_t ball = { 0 };
    ball.active = 1;
    ball.w = ball.h = ball_size;
    ball.x  = FX(W / 2);
    ball.y  = FX(H / 2);
    ball.vx = FX(4);
    ball.vy = (fx_t)(arcade_rand() % 512) - 256 + FX(2);

    while (arcade_frame(&a)) {
        if (a.pressed & (PAD_BTN_SELECT | PAD_BTN_B))
            exit(0);   /* Back to the launcher */
        if (a.pressed & PAD_BTN_START)
            paused = !paused;
        if (a.pressed & PAD_BTN_Y) {
            two_player = !two_player;
            sfx_select();
        }

        if (!paused) {
            /* Player paddle: D-pad or analog stick */
            if (a.held & PAD_BTN_UP)    left_y -= paddle_speed;
            if (a.held & PAD_BTN_DOWN)  left_y += paddle_speed;
            if (a.pad.ly < -8000) left_y -= paddle_speed;
            if (a.pad.ly >  8000) left_y += paddle_speed;
            if (left_y < 0) left_y = 0;
            if (left_y > H - paddle_h) left_y = H - paddle_h;

            if (two_player) {
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
            if (bx > W)           { score_l++; sound(700, 150); ball.x = FX(W/2); ball.y = FX(H/2); ball.vx = -FX(4); ball.vy = (fx_t)(arcade_rand() % 512) - 256; }
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

        surf_draw_text(&a.screen, W / 2 - 32, 76, two_player ? "2P MODE" : "VS CPU",
                       two_player ? rgb(120, 255, 160) : rgb(90, 100, 150),
                       SURF_TRANSPARENT, 1);
        surf_draw_text(&a.screen, 16, H - 16,
                       "SELECT/B: QUIT  START: PAUSE  Y: 1P/2P  P2: W/S",
                       rgb(80, 90, 140), SURF_TRANSPARENT, 1);
    }

    return 0;
}
