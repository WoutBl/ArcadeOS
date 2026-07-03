/*
 * ArcadeOS – Game Launcher (Ring 3 init process, the console "home screen")
 *
 * Lists the game binaries on the FAT32 volume (/games), lets the player
 * pick one with the D-pad, and launches it with spawn()/wait(). When the
 * game exits, the launcher takes the screen back.
 */

#include "../libc/syscall.h"
#include "../libc/string.h"
#include "../libc/console.h"

#define MAX_W 1024
#define MAX_H 768
#define MAX_GAMES 16

/* The launcher's private framebuffer (presented via SYS_GFX_PRESENT) */
static uint32_t framebuf[MAX_W * MAX_H];

typedef struct {
    char name[64];
    unsigned int size;
} game_entry_t;

static game_entry_t games[MAX_GAMES];
static int num_games = 0;

static int ends_with_elf(const char* name) {
    int len = (int)strlen(name);
    return len > 4 && strcmp(name + len - 4, ".ELF") == 0;
}

static void scan_games(void) {
    num_games = 0;
    dirent_info_t de;

    for (int i = 0; readdir_at("/games", i, &de) == 0 && num_games < MAX_GAMES; i++) {
        if (!(de.flags & 0x01)) continue;               /* Files only */
        if (!ends_with_elf(de.name)) continue;          /* Game binaries only */
        if (strcmp(de.name, "LAUNCHER.ELF") == 0) continue;

        strcpy(games[num_games].name, de.name);
        games[num_games].size = de.size;
        num_games++;
    }
}

static void draw_ui(surface_t* s, int selected, unsigned int t) {
    surf_clear(s, rgb(10, 12, 34));

    /* Header */
    surf_fill_rect(s, 0, 0, s->w, 64, rgb(18, 22, 60));
    surf_fill_rect(s, 0, 64, s->w, 3, rgb(80, 120, 255));
    surf_draw_text(s, 24, 20, "ARCADE OS", rgb(255, 255, 255), SURF_TRANSPARENT, 3);
    surf_draw_text(s, s->w - 168, 28, "GAME LIBRARY", rgb(120, 140, 220), SURF_TRANSPARENT, 1);

    /* Game list */
    int y = 100;
    if (num_games == 0) {
        surf_draw_text(s, 40, y, "NO GAMES FOUND ON /games",
                       rgb(255, 100, 100), SURF_TRANSPARENT, 2);
    }

    for (int i = 0; i < num_games; i++) {
        int row_h = 40;
        if (i == selected) {
            /* Pulsing highlight bar */
            int pulse = (int)((t / 16) % 64);
            if (pulse > 32) pulse = 64 - pulse;
            surf_fill_rect(s, 24, y - 8, s->w - 48, row_h,
                           rgb(30, (uint8_t)(50 + pulse), 130));
            surf_draw_rect(s, 24, y - 8, s->w - 48, row_h, rgb(120, 160, 255));
            surf_draw_text(s, 36, y, ">", rgb(255, 220, 80), SURF_TRANSPARENT, 2);
        }
        surf_draw_text(s, 64, y, games[i].name,
                       i == selected ? rgb(255, 255, 255) : rgb(150, 160, 200),
                       SURF_TRANSPARENT, 2);
        y += 48;
    }

    /* Footer */
    surf_fill_rect(s, 0, s->h - 36, s->w, 36, rgb(18, 22, 60));
    surf_draw_text(s, 24, s->h - 24, "UP/DOWN: SELECT   A(X KEY)/START: PLAY",
                   rgb(120, 140, 220), SURF_TRANSPARENT, 1);
}

int main(void) {
    gfx_info_t info;
    if (gfx_info(&info) != 0 || info.width * info.height > MAX_W * MAX_H) {
        write(1, "launcher: no usable framebuffer\n", 32);
        exit(1);
    }

    surface_t screen = { framebuf, (int)info.width, (int)info.height };

    scan_games();

    int selected = 0;
    unsigned short prev_buttons = 0;

    while (1) {
        pad_state_t pad;
        pad_read(0, &pad);

        /* Edge-detect button presses */
        unsigned short pressed = (unsigned short)(pad.buttons & ~prev_buttons);
        prev_buttons = pad.buttons;

        if ((pressed & PAD_BTN_DOWN) && num_games > 0) {
            selected = (selected + 1) % num_games;
            sound(600, 30);
        }
        if ((pressed & PAD_BTN_UP) && num_games > 0) {
            selected = (selected + num_games - 1) % num_games;
            sound(600, 30);
        }

        if ((pressed & (PAD_BTN_A | PAD_BTN_START)) && num_games > 0) {
            sound(900, 80);
            char path[80] = "/games/";
            strcpy(path + 7, games[selected].name);

            char* game_argv[] = { path, (char*)0 };
            int pid = spawn(path, game_argv);
            if (pid >= 0) {
                wait(pid);          /* Blocked until the game exits */
                prev_buttons = 0xFFFF;   /* Swallow buttons held over the transition */
                scan_games();
            }
        }

        draw_ui(&screen, selected, ticks());
        gfx_present(framebuf);
        msleep(16);   /* ~60 fps */
    }

    return 0;
}
