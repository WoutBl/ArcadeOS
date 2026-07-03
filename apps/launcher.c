/*
 * ArcadeOS – Game Launcher (Ring 3 init process, the console "home screen")
 *
 * Lists the game binaries on the FAT32 volume (/games), lets the player
 * pick one with the D-pad, and launches it with spawn()/wait(). When the
 * game exits, the launcher takes the screen back.
 */

#include "../sdk/arcade.h"
#include "../libc/syscall.h"
#include "../libc/string.h"

#define MAX_GAMES 16

typedef struct {
    char name[64];        /* Filename on the volume (8.3) */
    char title[32];       /* Pretty display name */
    unsigned int size;
} game_entry_t;

static game_entry_t games[MAX_GAMES];
static int num_games = 0;

/* Last-played persistence (launcher save slot 0) */
#define LP_MAGIC 0x4C41554Eu
typedef struct { unsigned int magic; char name[64]; } lastplayed_t;

/* Pretty titles: the 8.3 filesystem truncates long names, so map the
 * known ones back; everything else just loses the .ELF extension. */
static void pretty_title(const char* file, char* out) {
    static const char* fixups[][2] = {
        { "STARCATC.ELF", "STARCATCH" },
    };
    for (unsigned int i = 0; i < sizeof(fixups)/sizeof(fixups[0]); i++) {
        if (strcmp(file, fixups[i][0]) == 0) { strcpy(out, fixups[i][1]); return; }
    }
    int n = 0;
    while (file[n] && file[n] != '.' && n < 31) { out[n] = file[n]; n++; }
    out[n] = '\0';
}

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
        pretty_title(de.name, games[num_games].title);
        games[num_games].size = de.size;
        num_games++;
    }
}

static void draw_ui(surface_t* s, int selected, int last_idx, unsigned int t) {
    surf_clear(s, rgb(10, 12, 34));

    /* Header */
    surf_fill_rect(s, 0, 0, s->w, 64, rgb(18, 22, 60));
    surf_fill_rect(s, 0, 64, s->w, 3, rgb(80, 120, 255));
    surf_draw_text(s, 24, 20, "ARCADE OS", rgb(255, 255, 255), SURF_TRANSPARENT, 3);
    surf_draw_text(s, s->w - 168, 22, "GAME LIBRARY", rgb(120, 140, 220), SURF_TRANSPARENT, 1);
    {
        char cnt[16];
        int n = 0;
        int v = num_games;
        if (v == 0) cnt[n++] = '0';
        while (v && n < 8) { cnt[n++] = (char)('0' + v % 10); v /= 10; }
        for (int i = 0; i < n / 2; i++) { char c = cnt[i]; cnt[i] = cnt[n-1-i]; cnt[n-1-i] = c; }
        strcpy(cnt + n, " GAMES");
        surf_draw_text(s, s->w - 168, 38, cnt, rgb(90, 105, 170), SURF_TRANSPARENT, 1);
    }

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
        surf_draw_text(s, 64, y, games[i].title,
                       i == selected ? rgb(255, 255, 255) : rgb(150, 160, 200),
                       SURF_TRANSPARENT, 2);

        /* Size (KiB), right aligned; LAST PLAYED badge */
        {
            char kb[16];
            unsigned int v = games[i].size / 1024;
            if (v == 0) v = 1;
            int n = 0;
            while (v && n < 8) { kb[n++] = (char)('0' + v % 10); v /= 10; }
            for (int j = 0; j < n / 2; j++) { char c = kb[j]; kb[j] = kb[n-1-j]; kb[n-1-j] = c; }
            strcpy(kb + n, " KB");
            surf_draw_text(s, s->w - 104, y + 4, kb, rgb(90, 105, 170), SURF_TRANSPARENT, 1);
        }
        if (i == last_idx)
            surf_draw_text(s, s->w - 204, y + 4, "LAST PLAYED",
                           rgb(120, 220, 160), SURF_TRANSPARENT, 1);
        y += 48;
    }

    /* Footer */
    surf_fill_rect(s, 0, s->h - 36, s->w, 36, rgb(18, 22, 60));
    surf_draw_text(s, 24, s->h - 24, "UP/DOWN: SELECT   A(X KEY)/START: PLAY",
                   rgb(120, 140, 220), SURF_TRANSPARENT, 1);
}

int main(void) {
    arcade_t a;
    if (arcade_init(&a) != 0) {
        write(1, "launcher: no usable framebuffer\n", 32);
        exit(1);
    }

    scan_games();

    /* Resume on the last-played game */
    int selected = 0;
    int last_idx = -1;
    {
        lastplayed_t lp;
        if (arcade_load("LAUNCH", 0, &lp, sizeof(lp)) == (int)sizeof(lp) &&
            lp.magic == LP_MAGIC) {
            for (int i = 0; i < num_games; i++) {
                if (strcmp(games[i].name, lp.name) == 0) {
                    selected = i;
                    last_idx = i;
                    break;
                }
            }
        }
    }

    while (arcade_frame(&a)) {
        if ((a.pressed & PAD_BTN_DOWN) && num_games > 0) {
            selected = (selected + 1) % num_games;
            sfx_move();
        }
        if ((a.pressed & PAD_BTN_UP) && num_games > 0) {
            selected = (selected + num_games - 1) % num_games;
            sfx_move();
        }

        if ((a.pressed & (PAD_BTN_A | PAD_BTN_START)) && num_games > 0) {
            sfx_select();
            char path[80] = "/games/";
            strcpy(path + 7, games[selected].name);

            /* Remember the choice before handing over the console */
            lastplayed_t lp;
            lp.magic = LP_MAGIC;
            strcpy(lp.name, games[selected].name);
            arcade_save("LAUNCH", 0, &lp, sizeof(lp));
            last_idx = selected;

            char* game_argv[] = { path, (char*)0 };
            int pid = spawn(path, game_argv);
            if (pid >= 0) {
                wait(pid);          /* Blocked until the game exits */
                a.pad.buttons = 0xFFFF;  /* Swallow buttons held over the transition */
                scan_games();
            }
        }

        draw_ui(&a.screen, selected, last_idx, ticks());
    }

    return 0;
}
