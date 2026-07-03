/*
 * ArcadeOS – BREAKOUT (Ring 3 game)
 *
 * Controls (pad 0): D-pad or left stick = move paddle
 * START = launch ball / pause, SELECT or B = quit to launcher
 */

#include "../libc/syscall.h"
#include "../libc/string.h"
#include "../libc/console.h"

#define MAX_W 1024
#define MAX_H 768

/* Brick Grid Config
 * Only the row/column counts are compile-time; the pixel geometry is
 * derived from the real screen size in main() (gfx_info), because
 * MAX_W/MAX_H are just the static buffer bounds, not the resolution. */
#define BRICK_ROWS 6
#define BRICK_COLS 12
#define BRICK_H 24
#define BRICK_PAD 4

static uint32_t framebuf[MAX_W * MAX_H];
static uint8_t bricks[BRICK_ROWS][BRICK_COLS];

/* Persistent high score (FAT32 save data) */
#define SAVE_MAGIC 0xA2CADE01u
typedef struct {
    unsigned int magic;
    int          high;
} save_t;

static int load_high(void) {
    save_t sv;
    if (load_data("BREAKOUT.SAV", &sv, sizeof(sv)) == (int)sizeof(sv) &&
        sv.magic == SAVE_MAGIC)
        return sv.high;
    return 0;
}

static void save_high(int high) {
    save_t sv = { SAVE_MAGIC, high };
    save_data("BREAKOUT.SAV", &sv, sizeof(sv));
}

/* Fixed-point (8.8) ball state */
typedef struct {
    int x, y;      
    int dx, dy;    
} ball_t;

/* PRNG */
static unsigned int rand_state = 0x55AAFF00;
static unsigned int prand(void) {
    rand_state = rand_state * 1103515245 + 12345;
    return (rand_state >> 16) & 0x7FFF;
}

/* Integer-to-string score drawer */
static void draw_score(surface_t* s, int x, int y, int score, uint32_t color, int scale) {
    char buf[16];
    int i = 0;
    if (score == 0) { buf[i++] = '0'; } 
    else {
        int temp = score;
        int divisor = 1;
        while (temp / divisor >= 10) divisor *= 10;
        while (divisor > 0) {
            buf[i++] = (char)('0' + (temp / divisor));
            temp %= divisor;
            divisor /= 10;
        }
    }
    buf[i] = '\0';
    surf_draw_text(s, x, y, buf, color, SURF_TRANSPARENT, scale);
}

/* Reset the grid */
static void init_bricks(void) {
    for (int r = 0; r < BRICK_ROWS; r++) {
        for (int c = 0; c < BRICK_COLS; c++) {
            bricks[r][c] = 1; /* 1 = active, 0 = broken */
        }
    }
}

/* Get color based on row */
static uint32_t get_brick_color(int row) {
    switch (row) {
        case 0: return rgb(255, 60, 60);   /* Red */
        case 1: return rgb(255, 140, 40);  /* Orange */
        case 2: return rgb(255, 220, 40);  /* Yellow */
        case 3: return rgb(60, 255, 60);   /* Green */
        case 4: return rgb(60, 180, 255);  /* Blue */
        case 5: return rgb(180, 60, 255);  /* Purple */
        default: return rgb(200, 200, 200);
    }
}

int main(void) {
    gfx_info_t info;
    if (gfx_info(&info) != 0 || info.width * info.height > MAX_W * MAX_H) {
        write(1, "breakout: no usable framebuffer\n", 32);
        exit(1);
    }

    int W = (int)info.width;
    int H = (int)info.height;
    surface_t screen = { framebuf, W, H };

    /* Brick grid sized to the actual screen: side margins of 16 px,
     * bricks fill the rest. On 640x480 this gives 47-px bricks. */
    const int brick_w = (W - 32 - (BRICK_COLS - 1) * BRICK_PAD) / BRICK_COLS;
    const int grid_x  = (W - (BRICK_COLS * brick_w + (BRICK_COLS - 1) * BRICK_PAD)) / 2;
    const int grid_y  = 80;

    /* Game Constants */
    const int paddle_w = 100, paddle_h = 12;
    const int ball_size = 10;
    const int paddle_speed = 8;

    /* Game State */
    int paddle_x = W / 2 - paddle_w / 2;
    int paddle_y = H - 60;
    
    ball_t ball;
    int ball_active = 0;
    
    int score = 0;
    int high = load_high();
    int lives = 3;
    int paused = 0;
    int game_over = 0;
    int bricks_remaining = BRICK_ROWS * BRICK_COLS;

    init_bricks();
    unsigned short prev_buttons = 0;

    while (1) {
        pad_state_t pad;
        pad_read(0, &pad);

        unsigned short pressed = (unsigned short)(pad.buttons & ~prev_buttons);
        prev_buttons = pad.buttons;

        /* System Controls */
        if (pressed & (PAD_BTN_SELECT | PAD_BTN_B)) {
            if (score > high) save_high(score);
            exit(0);
        }
        
        if (pressed & PAD_BTN_START) {
            if (game_over || bricks_remaining == 0) {
                /* Hard Reset */
                lives = 3; score = 0; game_over = 0;
                init_bricks(); bricks_remaining = BRICK_ROWS * BRICK_COLS;
                ball_active = 0;
            } else if (!ball_active) {
                /* Launch Ball */
                ball_active = 1;
                ball.x = (paddle_x + paddle_w / 2 - ball_size / 2) << 8;
                ball.y = (paddle_y - ball_size - 2) << 8;
                ball.dx = (prand() % 512 - 256); /* Random slight angle */
                ball.dy = -(5 << 8); /* Launch upwards */
            } else {
                paused = !paused;
            }
        }

        if (!paused && !game_over && bricks_remaining > 0) {
            /* Paddle Movement */
            if (pad.buttons & PAD_BTN_LEFT) paddle_x -= paddle_speed;
            if (pad.buttons & PAD_BTN_RIGHT) paddle_x += paddle_speed;
            if (pad.lx < -8000) paddle_x -= paddle_speed;
            if (pad.lx >  8000) paddle_x += paddle_speed;

            /* Clamp Paddle */
            if (paddle_x < 0) paddle_x = 0;
            if (paddle_x > W - paddle_w) paddle_x = W - paddle_w;

            /* Ball Logic */
            if (ball_active) {
                ball.x += ball.dx;
                ball.y += ball.dy;

                int bx = ball.x >> 8;
                int by = ball.y >> 8;

                /* Left/Right Walls */
                if (bx <= 0) { ball.x = 0; ball.dx = -ball.dx; bx = 0; }
                if (bx >= W - ball_size) { ball.x = (W - ball_size) << 8; ball.dx = -ball.dx; bx = W - ball_size; }
                
                /* Top Wall */
                if (by <= 0) { ball.y = 0; ball.dy = -ball.dy; by = 0; }

                /* Bottom Wall (Death) */
                if (by >= H) {
                    ball_active = 0;
                    lives--;
                    if (lives <= 0) {
                        game_over = 1;
                        if (score > high) { high = score; save_high(high); }
                    }
                }

                /* Paddle Collision */
                if (ball.dy > 0 && 
                    bx + ball_size >= paddle_x && bx <= paddle_x + paddle_w &&
                    by + ball_size >= paddle_y && by <= paddle_y + paddle_h) {
                    
                    ball.y = (paddle_y - ball_size) << 8;
                    ball.dy = -ball.dy;
                    
                    /* English/Spin based on hit position */
                    int hit_pos = (bx + ball_size / 2) - (paddle_x + paddle_w / 2);
                    ball.dx += hit_pos << 4; 
                    
                    /* Clamp max horizontal speed */
                    if (ball.dx >  (7 << 8)) ball.dx =  (7 << 8);
                    if (ball.dx < -(7 << 8)) ball.dx = -(7 << 8);
                }

                /* Brick Collision */
                if (by >= grid_y && by <= grid_y + (BRICK_ROWS * (BRICK_H + BRICK_PAD))) {
                    int hit = 0;
                    for (int r = 0; r < BRICK_ROWS && !hit; r++) {
                        for (int c = 0; c < BRICK_COLS && !hit; c++) {
                            if (bricks[r][c]) {
                                int brk_x = grid_x + c * (brick_w + BRICK_PAD);
                                int brk_y = grid_y + r * (BRICK_H + BRICK_PAD);

                                if (bx + ball_size >= brk_x && bx <= brk_x + brick_w &&
                                    by + ball_size >= brk_y && by <= brk_y + BRICK_H) {
                                    
                                    /* Break brick */
                                    bricks[r][c] = 0;
                                    bricks_remaining--;
                                    score += (BRICK_ROWS - r) * 10; /* Higher bricks = more points */
                                    if (bricks_remaining == 0 && score > high) {
                                        high = score;   /* Board cleared: persist the win */
                                        save_high(high);
                                    }
                                    
                                    /* Simple bounce: invert Y */
                                    ball.dy = -ball.dy;
                                    hit = 1;
                                }
                            }
                        }
                    }
                }
            }
        }

        /* ──────── Render ──────── */
        surf_clear(&screen, rgb(10, 10, 25)); /* Very dark blue background */

        /* Draw Bricks */
        for (int r = 0; r < BRICK_ROWS; r++) {
            for (int c = 0; c < BRICK_COLS; c++) {
                if (bricks[r][c]) {
                    int brk_x = grid_x + c * (brick_w + BRICK_PAD);
                    int brk_y = grid_y + r * (BRICK_H + BRICK_PAD);
                    surf_fill_rect(&screen, brk_x, brk_y, brick_w, BRICK_H, get_brick_color(r));
                }
            }
        }

        /* Draw Paddle */
        surf_fill_rect(&screen, paddle_x, paddle_y, paddle_w, paddle_h, rgb(200, 200, 200));

        /* Draw Ball (or indicator if waiting to launch) */
        if (ball_active) {
            surf_fill_rect(&screen, ball.x >> 8, ball.y >> 8, ball_size, ball_size, rgb(255, 255, 255));
        } else if (!game_over && bricks_remaining > 0) {
            surf_fill_rect(&screen, paddle_x + paddle_w / 2 - ball_size / 2, paddle_y - ball_size - 2, 
                           ball_size, ball_size, rgb(255, 100, 100)); /* Red 'ready' ball */
        }

        /* Top HUD */
        surf_draw_text(&screen, 24, 24, "SCORE:", rgb(180, 180, 180), SURF_TRANSPARENT, 2);
        draw_score(&screen, 120, 24, score, rgb(255, 255, 255), 2);

        surf_draw_text(&screen, W / 2 - 56, 24, "HI:", rgb(180, 180, 120), SURF_TRANSPARENT, 2);
        draw_score(&screen, W / 2, 24, (score > high) ? score : high, rgb(255, 220, 80), 2);
        
        surf_draw_text(&screen, W - 160, 24, "LIVES:", rgb(180, 180, 180), SURF_TRANSPARENT, 2);
        draw_score(&screen, W - 48, 24, lives, rgb(255, 100, 100), 2);

        /* Game Over / Win States */
        if (game_over) {
            surf_draw_text(&screen, W / 2 - 120, H / 2 - 24, "GAME OVER", rgb(255, 80, 80), SURF_TRANSPARENT, 4);
            surf_draw_text(&screen, W / 2 - 130, H / 2 + 32, "START TO RESTART", rgb(200, 200, 200), SURF_TRANSPARENT, 2);
        } else if (bricks_remaining == 0) {
            surf_draw_text(&screen, W / 2 - 120, H / 2 - 24, "YOU WIN!", rgb(80, 255, 80), SURF_TRANSPARENT, 4);
            surf_draw_text(&screen, W / 2 - 130, H / 2 + 32, "START TO RESTART", rgb(200, 200, 200), SURF_TRANSPARENT, 2);
        } else if (paused) {
            surf_draw_text(&screen, W / 2 - 80, H / 2 - 16, "PAUSED", rgb(255, 220, 80), SURF_TRANSPARENT, 4);
        }

        /* Controls Footer */
        surf_draw_text(&screen, 16, H - 24, "SELECT/B: QUIT  START: LAUNCH/PAUSE", rgb(80, 90, 140), SURF_TRANSPARENT, 1);

        gfx_present(framebuf);
        msleep(16);   
    }

    return 0;
}