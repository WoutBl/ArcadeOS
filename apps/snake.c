/*
 * ArcadeOS – SNAKE (Ring 3 game)
 *
 * Controls (pad 0): D-pad or left stick = change direction
 * START = pause/restart, SELECT or B = quit to launcher
 * * Features: Screen wrapping and collision-safe apple spawning.
 */

#include "../libc/syscall.h"
#include "../libc/string.h"
#include "../libc/console.h"

#define MAX_W 1024
#define MAX_H 768
#define TILE_SIZE 20
#define MAX_SNAKE 2048

static uint32_t framebuf[MAX_W * MAX_H];
static int snake_x[MAX_SNAKE];
static int snake_y[MAX_SNAKE];

/* Persistent high score (FAT32 save data) */
#define SAVE_MAGIC 0xA2CADE02u
typedef struct {
    unsigned int magic;
    int          high;
} save_t;

static int load_high(void) {
    save_t sv;
    if (load_data("SNAKE.SAV", &sv, sizeof(sv)) == (int)sizeof(sv) &&
        sv.magic == SAVE_MAGIC)
        return sv.high;
    return 0;
}

static void save_high(int high) {
    save_t sv = { SAVE_MAGIC, high };
    save_data("SNAKE.SAV", &sv, sizeof(sv));
}

static unsigned int rand_state = 0x89ABCDEF;

static unsigned int prand(void) {
    rand_state = rand_state * 1103515245 + 12345;
    return (rand_state >> 16) & 0x7FFF;
}

static void draw_score(surface_t* s, int x, int y, int score, uint32_t color) {
    char buf[16];
    int i = 0;
    
    if (score == 0) {
        buf[i++] = '0';
    } else {
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
    surf_draw_text(s, x, y, buf, color, SURF_TRANSPARENT, 4);
}

/* * Safely spawn an apple guaranteeing it doesn't land inside the snake 
 */
static void spawn_apple(int* ax, int* ay, int gw, int gh, int len) {
    int valid = 0;
    while (!valid) {
        *ax = prand() % gw;
        *ay = prand() % gh;
        valid = 1;
        for (int i = 0; i < len; i++) {
            if (*ax == snake_x[i] && *ay == snake_y[i]) {
                valid = 0;
                break;
            }
        }
    }
}

int main(void) {
    gfx_info_t info;
    if (gfx_info(&info) != 0 || info.width * info.height > MAX_W * MAX_H) {
        write(1, "snake: no usable framebuffer\n", 29);
        exit(1);
    }

    int W = (int)info.width;
    int H = (int)info.height;
    surface_t screen = { framebuf, W, H };

    int grid_w = W / TILE_SIZE;
    int grid_h = H / TILE_SIZE;

    /* Game State */
    int snake_len = 3;
    snake_x[0] = grid_w / 2;     snake_y[0] = grid_h / 2;
    snake_x[1] = grid_w / 2 - 1; snake_y[1] = grid_h / 2;
    snake_x[2] = grid_w / 2 - 2; snake_y[2] = grid_h / 2;

    int dir_x = 1, dir_y = 0;
    int next_dir_x = 1, next_dir_y = 0;

    int apple_x, apple_y;
    spawn_apple(&apple_x, &apple_y, grid_w, grid_h, snake_len);

    int score = 0;
    int high = load_high();
    int paused = 0;
    int game_over = 0;

    unsigned short prev_buttons = 0;
    
    int frame_counter = 0;
    int speed_threshold = 6; 

    while (1) {
        pad_state_t pad;
        pad_read(0, &pad);

        unsigned short pressed = (unsigned short)(pad.buttons & ~prev_buttons);
        prev_buttons = pad.buttons;

        if (pressed & (PAD_BTN_SELECT | PAD_BTN_B)) {
            if (score > high) save_high(score);
            exit(0);
        }
        
        if (pressed & PAD_BTN_START) {
            if (game_over) {
                game_over = 0; score = 0; snake_len = 3; speed_threshold = 6;
                dir_x = 1; dir_y = 0; next_dir_x = 1; next_dir_y = 0;
                snake_x[0] = grid_w / 2;     snake_y[0] = grid_h / 2;
                snake_x[1] = grid_w / 2 - 1; snake_y[1] = grid_h / 2;
                snake_x[2] = grid_w / 2 - 2; snake_y[2] = grid_h / 2;
                spawn_apple(&apple_x, &apple_y, grid_w, grid_h, snake_len);
            } else {
                paused = !paused;
            }
        }

        if (!paused && !game_over) {
            if ((pad.buttons & PAD_BTN_UP) && dir_y != 1)         { next_dir_x = 0;  next_dir_y = -1; }
            if ((pad.buttons & PAD_BTN_DOWN) && dir_y != -1)      { next_dir_x = 0;  next_dir_y = 1; }
            if ((pad.buttons & PAD_BTN_LEFT) && dir_x != 1)       { next_dir_x = -1; next_dir_y = 0; }
            if ((pad.buttons & PAD_BTN_RIGHT) && dir_x != -1)     { next_dir_x = 1;  next_dir_y = 0; }
            
            if (pad.ly < -8000 && dir_y != 1) { next_dir_x = 0;  next_dir_y = -1; }
            if (pad.ly >  8000 && dir_y != -1) { next_dir_x = 0;  next_dir_y = 1; }
            if (pad.lx < -8000 && dir_x != 1) { next_dir_x = -1; next_dir_y = 0; }
            if (pad.lx >  8000 && dir_x != -1) { next_dir_x = 1;  next_dir_y = 0; }

            frame_counter++;
            if (frame_counter >= speed_threshold) {
                frame_counter = 0;
                dir_x = next_dir_x;
                dir_y = next_dir_y;

                int new_head_x = snake_x[0] + dir_x;
                int new_head_y = snake_y[0] + dir_y;

                /* Screen Wrapping */
                if (new_head_x < 0) new_head_x = grid_w - 1;
                else if (new_head_x >= grid_w) new_head_x = 0;

                if (new_head_y < 0) new_head_y = grid_h - 1;
                else if (new_head_y >= grid_h) new_head_y = 0;

                /* Self Collision */
                for (int i = 0; i < snake_len; i++) {
                    if (new_head_x == snake_x[i] && new_head_y == snake_y[i]) {
                        game_over = 1;
                        sound(150, 400);
                        if (score > high) { high = score; save_high(high); }
                    }
                }

                if (!game_over) {
                    int old_tail_x = snake_x[snake_len - 1];
                    int old_tail_y = snake_y[snake_len - 1];

                    for (int i = snake_len - 1; i > 0; i--) {
                        snake_x[i] = snake_x[i - 1];
                        snake_y[i] = snake_y[i - 1];
                    }
                    snake_x[0] = new_head_x;
                    snake_y[0] = new_head_y;

                    /* Apple Collision */
                    if (new_head_x == apple_x && new_head_y == apple_y) {
                        if (snake_len < MAX_SNAKE) {
                            snake_x[snake_len] = old_tail_x;
                            snake_y[snake_len] = old_tail_y;
                            snake_len++;
                        }
                        score++;
                        sound(880, 50);
                        
                        spawn_apple(&apple_x, &apple_y, grid_w, grid_h, snake_len);
                        
                        if (speed_threshold > 2 && score % 10 == 0) {
                            speed_threshold--;
                        }
                    }
                }
            }
        }

        /* ──────── Render ──────── */
        surf_clear(&screen, rgb(15, 20, 15)); 

        surf_fill_rect(&screen, apple_x * TILE_SIZE + 2, apple_y * TILE_SIZE + 2, 
                       TILE_SIZE - 4, TILE_SIZE - 4, rgb(255, 60, 60));

        for (int i = 0; i < snake_len; i++) {
            uint32_t color = (i == 0) ? rgb(120, 255, 120) : rgb(60, 200, 60);
            surf_fill_rect(&screen, snake_x[i] * TILE_SIZE + 1, snake_y[i] * TILE_SIZE + 1, 
                           TILE_SIZE - 2, TILE_SIZE - 2, color);
        }

        draw_score(&screen, 24, 24, score, rgb(255, 255, 255));

        surf_draw_text(&screen, W - 170, 24, "HI", rgb(160, 180, 120), SURF_TRANSPARENT, 2);
        {
            char hbuf[16];
            int hv = (score > high) ? score : high;
            int hi = 0;
            if (hv == 0) hbuf[hi++] = '0';
            else {
                int divisor = 1;
                while (hv / divisor >= 10) divisor *= 10;
                while (divisor > 0) {
                    hbuf[hi++] = (char)('0' + (hv / divisor));
                    hv %= divisor;
                    divisor /= 10;
                }
            }
            hbuf[hi] = '\0';
            surf_draw_text(&screen, W - 120, 16, hbuf, rgb(255, 220, 80), SURF_TRANSPARENT, 3);
        }

        if (game_over) {
            surf_draw_text(&screen, W / 2 - 120, H / 2 - 24, "GAME OVER", 
                           rgb(255, 80, 80), SURF_TRANSPARENT, 4);
            surf_draw_text(&screen, W / 2 - 130, H / 2 + 32, "START TO RESTART", 
                           rgb(200, 200, 200), SURF_TRANSPARENT, 2); /* <-- Your file ended right here! */
        } else if (paused) {
            surf_draw_text(&screen, W / 2 - 80, H / 2 - 16, "PAUSED", 
                           rgb(255, 220, 80), SURF_TRANSPARENT, 4);
        }

        surf_draw_text(&screen, 16, H - 24, "SELECT/B: QUIT  START: PAUSE", 
                       rgb(80, 100, 80), SURF_TRANSPARENT, 1);

        gfx_present(framebuf);
        msleep(16);   
    } /* Closes while(1) */

    return 0;
} /* Closes main() */