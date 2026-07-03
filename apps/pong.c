/*
 * ArcadeOS – PONG (Ring 3 game)
 *
 * The reference title proving the full console loop:
 * FAT32 load → ELF exec → Ring 3 → pad input syscall → gfx present syscall.
 *
 * Controls (pad 0): D-pad up/down or left stick = move paddle
 *                   START = pause, SELECT or B = quit to launcher
 */

#include "../libc/syscall.h"
#include "../libc/string.h"
#include "../libc/console.h"

#define MAX_W 1024
#define MAX_H 768

static uint32_t framebuf[MAX_W * MAX_H];

/* Fixed-point (8.8) ball state keeps the math integer-only */
typedef struct {
    int x, y;      /* 8.8 fixed */
    int dx, dy;    /* 8.8 fixed per frame */
} ball_t;

static unsigned int rand_state = 0x1234567;

static unsigned int prand(void) {
    rand_state = rand_state * 1103515245 + 12345;
    return (rand_state >> 16) & 0x7FFF;
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
    gfx_info_t info;
    if (gfx_info(&info) != 0 || info.width * info.height > MAX_W * MAX_H) {
        write(1, "pong: no usable framebuffer\n", 28);
        exit(1);
    }

    int W = (int)info.width;
    int H = (int)info.height;
    surface_t screen = { framebuf, W, H };

    const int paddle_w = 12, paddle_h = 80;
    const int ball_size = 10;
    const int paddle_speed = 6;

    int left_y  = H / 2 - paddle_h / 2;
    int right_y = H / 2 - paddle_h / 2;
    int score_l = 0, score_r = 0;
    int paused = 0;

    ball_t ball;
    ball.x = (W / 2) << 8;
    ball.y = (H / 2) << 8;
    ball.dx = 4 << 8;
    ball.dy = (int)((prand() % 512)) - 256 + (2 << 8);

    unsigned short prev_buttons = 0;

    while (1) {
        pad_state_t pad;
        pad_read(0, &pad);

        unsigned short pressed = (unsigned short)(pad.buttons & ~prev_buttons);
        prev_buttons = pad.buttons;

        if (pressed & (PAD_BTN_SELECT | PAD_BTN_B))
            exit(0);   /* Back to the launcher */
        if (pressed & PAD_BTN_START)
            paused = !paused;

        if (!paused) {
            /* Player paddle: D-pad or analog stick */
            if (pad.buttons & PAD_BTN_UP)    left_y -= paddle_speed;
            if (pad.buttons & PAD_BTN_DOWN)  left_y += paddle_speed;
            if (pad.ly < -8000) left_y -= paddle_speed;
            if (pad.ly >  8000) left_y += paddle_speed;
            if (left_y < 0) left_y = 0;
            if (left_y > H - paddle_h) left_y = H - paddle_h;

            /* AI paddle: track the ball with a speed cap */
            int ball_cy = (ball.y >> 8) + ball_size / 2;
            int ai_cy   = right_y + paddle_h / 2;
            if (ai_cy < ball_cy - 8) right_y += paddle_speed - 2;
            if (ai_cy > ball_cy + 8) right_y -= paddle_speed - 2;
            if (right_y < 0) right_y = 0;
            if (right_y > H - paddle_h) right_y = H - paddle_h;

            /* Ball physics */
            ball.x += ball.dx;
            ball.y += ball.dy;

            int bx = ball.x >> 8;
            int by = ball.y >> 8;

            /* Top/bottom walls */
            if (by <= 0)              { ball.y = 0;                       ball.dy = -ball.dy; }
            if (by >= H - ball_size)  { ball.y = (H - ball_size) << 8;    ball.dy = -ball.dy; }

            /* Left paddle */
            if (bx <= 24 + paddle_w && bx >= 24 &&
                by + ball_size >= left_y && by <= left_y + paddle_h &&
                ball.dx < 0) {
                ball.dx = -ball.dx + 32;    /* Speed up slightly each hit */
                /* Add english based on where the paddle was struck */
                ball.dy += ((by + ball_size / 2) - (left_y + paddle_h / 2)) << 4;
                sound(440, 40);
            }

            /* Right paddle */
            if (bx + ball_size >= W - 24 - paddle_w && bx + ball_size <= W - 24 &&
                by + ball_size >= right_y && by <= right_y + paddle_h &&
                ball.dx > 0) {
                ball.dx = -(ball.dx + 32);
                ball.dy += ((by + ball_size / 2) - (right_y + paddle_h / 2)) << 4;
                sound(440, 40);
            }

            /* Clamp vertical speed */
            if (ball.dy >  6 << 8) ball.dy =  6 << 8;
            if (ball.dy < -(6 << 8)) ball.dy = -(6 << 8);

            /* Scoring */
            if (bx < -ball_size)  { score_r++; sound(180, 250); ball.x = (W/2) << 8; ball.y = (H/2) << 8; ball.dx =  4 << 8; ball.dy = (int)(prand() % 512) - 256; }
            if (bx > W)           { score_l++; sound(700, 150); ball.x = (W/2) << 8; ball.y = (H/2) << 8; ball.dx = -(4 << 8); ball.dy = (int)(prand() % 512) - 256; }
        }

        /* ──────── Render ──────── */
        surf_clear(&screen, rgb(6, 6, 18));

        /* Center line */
        for (int y = 0; y < H; y += 24)
            surf_fill_rect(&screen, W / 2 - 2, y, 4, 12, rgb(60, 70, 120));

        draw_score(&screen, W / 2 - 96, score_l, rgb(120, 200, 255));
        draw_score(&screen, W / 2 + 40, score_r, rgb(255, 160, 120));

        surf_fill_rect(&screen, 24, left_y, paddle_w, paddle_h, rgb(120, 200, 255));
        surf_fill_rect(&screen, W - 24 - paddle_w, right_y, paddle_w, paddle_h, rgb(255, 160, 120));
        surf_fill_rect(&screen, ball.x >> 8, ball.y >> 8, ball_size, ball_size, rgb(255, 255, 255));

        if (paused)
            surf_draw_text(&screen, W / 2 - 96, H / 2 - 16, "PAUSED",
                           rgb(255, 220, 80), SURF_TRANSPARENT, 4);

        surf_draw_text(&screen, 16, H - 16, "SELECT/B: QUIT  START: PAUSE",
                       rgb(80, 90, 140), SURF_TRANSPARENT, 1);

        gfx_present(framebuf);
        msleep(16);   /* ~60 fps */
    }

    return 0;
}
