/*
 * ArcadeOS – BREAKOUT (Ring 3 game)
 *
 * Controls (pad 0): D-pad or left stick = move paddle
 * START = launch ball / pause, SELECT or B = quit to launcher
 */

#include "../sdk/arcade.h"
#include "../libc/syscall.h"

/* Brick Grid Config
 * Only the row/column counts are compile-time; the pixel geometry is
 * derived from the real screen size in main() (gfx_info), because
 * MAX_W/MAX_H are just the static buffer bounds, not the resolution. */
#define BRICK_ROWS 6
#define BRICK_COLS 12
#define BRICK_H 24
#define BRICK_PAD 4

static uint8_t bricks[BRICK_ROWS][BRICK_COLS];

/* Persistent high score (SDK save slot 0 -> BREAKOU0.SAV) */
#define SAVE_MAGIC 0xA2CADE01u
typedef struct {
    unsigned int magic;
    int          high;
} save_t;

static int load_high(void) {
    save_t sv;
    if (arcade_load("BREAKOUT", 0, &sv, sizeof(sv)) == (int)sizeof(sv) &&
        sv.magic == SAVE_MAGIC)
        return sv.high;
    return 0;
}

static void save_high(int high) {
    save_t sv = { SAVE_MAGIC, high };
    arcade_save("BREAKOUT", 0, &sv, sizeof(sv));
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
    arcade_t a;
    if (arcade_init(&a) != 0) {
        write(1, "breakout: no usable framebuffer\n", 32);
        exit(1);
    }

    int W = a.w;
    int H = a.h;

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
    
    entity_t ball = { 0 };
    ball.w = ball.h = ball_size;
    int ball_active = 0;
    
    int score = 0;
    int high = load_high();
    int lives = 3;
    int paused = 0;
    int game_over = 0;
    int bricks_remaining = BRICK_ROWS * BRICK_COLS;

    init_bricks();

    while (arcade_frame(&a)) {
        a.score = score;
        /* System Controls */
        if (a.pressed & (PAD_BTN_SELECT | PAD_BTN_B)) {
            if (score > high) save_high(score);
            exit(0);
        }
        
        if (a.pressed & PAD_BTN_START) {
            if (game_over || bricks_remaining == 0) {
                /* Hard Reset */
                lives = 3; score = 0; game_over = 0;
                init_bricks(); bricks_remaining = BRICK_ROWS * BRICK_COLS;
                ball_active = 0;
            } else if (!ball_active) {
                /* Launch Ball */
                ball_active = 1;
                ball.active = 1;
                ball.x = FX(paddle_x + paddle_w / 2 - ball_size / 2);
                ball.y = FX(paddle_y - ball_size - 2);
                ball.vx = (fx_t)(arcade_rand() % 512) - 256; /* Random slight angle */
                ball.vy = -FX(5); /* Launch upwards */
            } else {
                paused = !paused;
            }
        }

        if (!paused && !game_over && bricks_remaining > 0) {
            /* Paddle Movement */
            if (a.held & PAD_BTN_LEFT) paddle_x -= paddle_speed;
            if (a.held & PAD_BTN_RIGHT) paddle_x += paddle_speed;
            if (a.pad.lx < -8000) paddle_x -= paddle_speed;
            if (a.pad.lx >  8000) paddle_x += paddle_speed;

            /* Clamp Paddle */
            if (paddle_x < 0) paddle_x = 0;
            if (paddle_x > W - paddle_w) paddle_x = W - paddle_w;

            /* Ball Logic */
            if (ball_active) {
                arcade_entity_move(&ball);

                int bx = FX_INT(ball.x);
                int by = FX_INT(ball.y);

                /* Left/Right Walls */
                if (bx <= 0) { ball.x = 0; ball.vx = -ball.vx; bx = 0; }
                if (bx >= W - ball_size) { ball.x = FX(W - ball_size); ball.vx = -ball.vx; bx = W - ball_size; }
                
                /* Top Wall */
                if (by <= 0) { ball.y = 0; ball.vy = -ball.vy; by = 0; }

                /* Bottom Wall (Death) */
                if (by >= H) {
                    ball_active = 0;
                    lives--;
                    sfx_lose();
                    if (lives <= 0) {
                        game_over = 1;
                        if (score > high) { high = score; save_high(high); }
                    }
                }

                /* Paddle Collision */
                if (ball.vy > 0 &&
                    arcade_aabb(bx, by, ball_size, ball_size,
                                paddle_x, paddle_y, paddle_w, paddle_h)) {
                    
                    ball.y = FX(paddle_y - ball_size);
                    ball.vy = -ball.vy;
                    sound(400, 30);
                    
                    /* English/Spin based on hit position */
                    int hit_pos = (bx + ball_size / 2) - (paddle_x + paddle_w / 2);
                    ball.vx += hit_pos << 4; 
                    
                    /* Clamp max horizontal speed */
                    if (ball.vx >  FX(7)) ball.vx =  FX(7);
                    if (ball.vx < -FX(7)) ball.vx = -FX(7);
                }

                /* Brick Collision */
                if (by >= grid_y && by <= grid_y + (BRICK_ROWS * (BRICK_H + BRICK_PAD))) {
                    int hit = 0;
                    for (int r = 0; r < BRICK_ROWS && !hit; r++) {
                        for (int c = 0; c < BRICK_COLS && !hit; c++) {
                            if (bricks[r][c]) {
                                int brk_x = grid_x + c * (brick_w + BRICK_PAD);
                                int brk_y = grid_y + r * (BRICK_H + BRICK_PAD);

                                if (arcade_aabb(bx, by, ball_size, ball_size,
                                                brk_x, brk_y, brick_w, BRICK_H)) {
                                    
                                    /* Break brick */
                                    bricks[r][c] = 0;
                                    bricks_remaining--;
                                    sound(700 + (BRICK_ROWS - r) * 60, 30);
                                    score += (BRICK_ROWS - r) * 10; /* Higher bricks = more points */
                                    if (bricks_remaining == 0 && score > high) {
                                        high = score;   /* Board cleared: persist the win */
                                        save_high(high);
                                    }
                                    
                                    /* Simple bounce: invert Y */
                                    ball.vy = -ball.vy;
                                    hit = 1;
                                }
                            }
                        }
                    }
                }
            }
        }

        /* ──────── Render ──────── */
        surf_clear(&a.screen, rgb(10, 10, 25)); /* Very dark blue background */

        /* Draw Bricks */
        for (int r = 0; r < BRICK_ROWS; r++) {
            for (int c = 0; c < BRICK_COLS; c++) {
                if (bricks[r][c]) {
                    int brk_x = grid_x + c * (brick_w + BRICK_PAD);
                    int brk_y = grid_y + r * (BRICK_H + BRICK_PAD);
                    surf_fill_rect(&a.screen, brk_x, brk_y, brick_w, BRICK_H, get_brick_color(r));
                }
            }
        }

        /* Draw Paddle */
        surf_fill_rect(&a.screen, paddle_x, paddle_y, paddle_w, paddle_h, rgb(200, 200, 200));

        /* Draw Ball (or indicator if waiting to launch) */
        if (ball_active) {
            surf_fill_rect(&a.screen, FX_INT(ball.x), FX_INT(ball.y), ball_size, ball_size, rgb(255, 255, 255));
        } else if (!game_over && bricks_remaining > 0) {
            surf_fill_rect(&a.screen, paddle_x + paddle_w / 2 - ball_size / 2, paddle_y - ball_size - 2, 
                           ball_size, ball_size, rgb(255, 100, 100)); /* Red 'ready' ball */
        }

        /* Top HUD */
        surf_draw_text(&a.screen, 24, 24, "SCORE:", rgb(180, 180, 180), SURF_TRANSPARENT, 2);
        draw_score(&a.screen, 120, 24, score, rgb(255, 255, 255), 2);

        surf_draw_text(&a.screen, W / 2 - 56, 24, "HI:", rgb(180, 180, 120), SURF_TRANSPARENT, 2);
        draw_score(&a.screen, W / 2, 24, (score > high) ? score : high, rgb(255, 220, 80), 2);
        
        surf_draw_text(&a.screen, W - 160, 24, "LIVES:", rgb(180, 180, 180), SURF_TRANSPARENT, 2);
        draw_score(&a.screen, W - 48, 24, lives, rgb(255, 100, 100), 2);

        /* Game Over / Win States */
        if (game_over) {
            surf_draw_text(&a.screen, W / 2 - 120, H / 2 - 24, "GAME OVER", rgb(255, 80, 80), SURF_TRANSPARENT, 4);
            surf_draw_text(&a.screen, W / 2 - 130, H / 2 + 32, "START TO RESTART", rgb(200, 200, 200), SURF_TRANSPARENT, 2);
        } else if (bricks_remaining == 0) {
            surf_draw_text(&a.screen, W / 2 - 120, H / 2 - 24, "YOU WIN!", rgb(80, 255, 80), SURF_TRANSPARENT, 4);
            surf_draw_text(&a.screen, W / 2 - 130, H / 2 + 32, "START TO RESTART", rgb(200, 200, 200), SURF_TRANSPARENT, 2);
        } else if (paused) {
            surf_draw_text(&a.screen, W / 2 - 80, H / 2 - 16, "PAUSED", rgb(255, 220, 80), SURF_TRANSPARENT, 4);
        }

        /* Controls Footer */
        surf_draw_text(&a.screen, 16, H - 24, "SELECT/B: QUIT  START: LAUNCH/PAUSE", rgb(80, 90, 140), SURF_TRANSPARENT, 1);

    }

    return 0;
}