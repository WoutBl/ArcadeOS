/*
 * ArcadeOS – Game Launcher (Ring 3 init process, the console "home screen")
 *
 * Lists the game binaries on the FAT32 volume (/games), lets the player
 * pick one with the D-pad, and launches it with spawn()/wait(). When the
 * game exits, the launcher takes the screen back.
 *
 * Also owns the console's user profiles: names live in USERS0.SAV, an
 * on-screen keyboard adds new ones, and picking Player 1 (+ optional
 * Player 2) activates the session in the kernel (SYS_SESSION) so games
 * and the REST API see who is playing.
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

/* ──────── User profiles (USERS0.SAV) ──────── */

#define US_MAGIC  0x55535253u   /* 'USRS' */
#define MAX_USERS 8

typedef struct {
    unsigned int magic;
    int  count;
    char names[MAX_USERS][SESSION_NAME_LEN];
    int  p1, p2;          /* Active player indices; -1 = none */
} users_t;

static users_t users;

/* Which screen the launcher is on */
enum { SCR_HOME, SCR_USERS, SCR_NAME, SCR_SCORES };
static int screen = SCR_HOME;

/* ──────── Central highscores (written by the kernel) ──────── */

static hiscore_file_t board;
static int board_ok = 0;

/* The kernel owns the live board and flushes it to HISCORE0.SAV from
 * the idle task (~2 s throttle), so reading the file on screen entry
 * is at most a couple of seconds behind the last game. */
static void board_load(void) {
    hiscore_file_t tmp;
    board_ok = 0;
    board.count = 0;
    if (arcade_load("HISCORE", 0, &tmp, sizeof(tmp)) < (int)(2 * sizeof(uint32_t)))
        return;
    if (tmp.magic != HISCORE_MAGIC || tmp.count > HISCORE_MAX)
        return;
    board = tmp;
    /* Sort by score, best first (tiny N: insertion sort) */
    for (uint32_t i = 1; i < board.count; i++) {
        hiscore_entry_t key = board.e[i];
        int j = (int)i - 1;
        while (j >= 0 && board.e[j].score < key.score) {
            board.e[j + 1] = board.e[j];
            j--;
        }
        board.e[j + 1] = key;
    }
    board_ok = 1;
}

static void users_load(void) {
    users_t tmp;
    if (arcade_load("USERS", 0, &tmp, sizeof(tmp)) == (int)sizeof(tmp) &&
        tmp.magic == US_MAGIC &&
        tmp.count >= 0 && tmp.count <= MAX_USERS) {
        users = tmp;
        if (users.p1 >= users.count) users.p1 = -1;
        if (users.p2 >= users.count) users.p2 = -1;
        return;
    }
    users.magic = US_MAGIC;
    users.count = 0;
    users.p1 = users.p2 = -1;
}

static void users_save(void) {
    arcade_save("USERS", 0, &users, sizeof(users));
}

/* Push the picked players into the kernel session */
static void apply_session(void) {
    session_req_t rq = {0};
    rq.op = SESSION_OP_SET;
    if (users.p1 >= 0) strcpy(rq.p1, users.names[users.p1]);
    else               strcpy(rq.p1, "GUEST");
    if (users.p2 >= 0 && users.p2 != users.p1) {
        rq.count = 2;
        strcpy(rq.p2, users.names[users.p2]);
    } else {
        rq.count = 1;
    }
    session_op(&rq);
}

/* ──────── Small helpers ──────── */

static void fmt_u(char* out, unsigned int v) {
    int n = 0;
    if (v == 0) out[n++] = '0';
    while (v && n < 10) { out[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < n / 2; i++) { char c = out[i]; out[i] = out[n-1-i]; out[n-1-i] = c; }
    out[n] = '\0';
}

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

/* ──────── Home screen ──────── */

static void draw_home(surface_t* s, int selected, int last_idx, unsigned int t) {
    surf_clear(s, rgb(10, 12, 34));

    /* Header */
    surf_fill_rect(s, 0, 0, s->w, 64, rgb(18, 22, 60));
    surf_fill_rect(s, 0, 64, s->w, 3, rgb(80, 120, 255));
    surf_draw_text(s, 24, 20, "ARCADE OS", rgb(255, 255, 255), SURF_TRANSPARENT, 3);

    /* Who is playing (top right) */
    {
        char line[40] = "P1 ";
        strcpy(line + 3, users.p1 >= 0 ? users.names[users.p1] : "GUEST");
        surf_draw_text(s, s->w - 168, 14, line, rgb(150, 200, 255), SURF_TRANSPARENT, 1);
        if (users.p2 >= 0 && users.p2 != users.p1) {
            char l2[40] = "P2 ";
            strcpy(l2 + 3, users.names[users.p2]);
            surf_draw_text(s, s->w - 168, 28, l2, rgb(255, 180, 140), SURF_TRANSPARENT, 1);
        }
        char cnt[16];
        fmt_u(cnt, (unsigned)num_games);
        strcpy(cnt + strlen(cnt), " GAMES");
        surf_draw_text(s, s->w - 168, 46, cnt, rgb(90, 105, 170), SURF_TRANSPARENT, 1);
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
            fmt_u(kb, v);
            strcpy(kb + strlen(kb), " KB");
            surf_draw_text(s, s->w - 104, y + 4, kb, rgb(90, 105, 170), SURF_TRANSPARENT, 1);
        }
        if (i == last_idx)
            surf_draw_text(s, s->w - 204, y + 4, "LAST PLAYED",
                           rgb(120, 220, 160), SURF_TRANSPARENT, 1);
        y += 48;
    }

    /* Footer */
    surf_fill_rect(s, 0, s->h - 36, s->w, 36, rgb(18, 22, 60));
    surf_draw_text(s, 24, s->h - 24,
                   "A(X): PLAY   Y(V): PLAYERS   X(C): SCORES",
                   rgb(120, 140, 220), SURF_TRANSPARENT, 1);
}

/* ──────── Scoreboard screen ──────── */

static void draw_scores(surface_t* s) {
    surf_clear(s, rgb(10, 12, 34));
    surf_fill_rect(s, 0, 0, s->w, 64, rgb(18, 40, 40));
    surf_fill_rect(s, 0, 64, s->w, 3, rgb(80, 220, 160));
    surf_draw_text(s, 24, 20, "HIGH SCORES", rgb(255, 255, 255), SURF_TRANSPARENT, 3);

    if (!board_ok || board.count == 0) {
        surf_draw_text(s, 40, 110, "NO SCORES YET - GO PLAY!",
                       rgb(120, 220, 160), SURF_TRANSPARENT, 2);
    } else {
        /* Column headers */
        surf_draw_text(s, 64, 84,  "GAME",   rgb(90, 140, 120), SURF_TRANSPARENT, 1);
        surf_draw_text(s, 260, 84, "PLAYER", rgb(90, 140, 120), SURF_TRANSPARENT, 1);
        surf_draw_text(s, s->w - 140, 84, "SCORE", rgb(90, 140, 120), SURF_TRANSPARENT, 1);

        int y = 104;
        uint32_t rows = board.count;
        uint32_t max_rows = (uint32_t)((s->h - 150) / 30);
        if (rows > max_rows) rows = max_rows;
        for (uint32_t i = 0; i < rows; i++) {
            uint32_t fg = (i == 0) ? rgb(255, 220, 80)
                        : (i == 1) ? rgb(210, 210, 220)
                        : (i == 2) ? rgb(220, 160, 110)
                        : rgb(150, 170, 190);
            char rank[8];
            fmt_u(rank, i + 1);
            surf_draw_text(s, 32, y, rank, fg, SURF_TRANSPARENT, 2);

            char game[13], user[SESSION_NAME_LEN];
            /* NUL-pad guards: the file comes off disk */
            for (int k = 0; k < 12; k++) game[k] = board.e[i].game[k];
            game[12] = '\0';
            for (int k = 0; k < SESSION_NAME_LEN; k++) user[k] = board.e[i].user[k];
            user[SESSION_NAME_LEN - 1] = '\0';

            surf_draw_text(s, 64, y, game, fg, SURF_TRANSPARENT, 2);
            surf_draw_text(s, 260, y, user, fg, SURF_TRANSPARENT, 2);

            char sc[16];
            int32_t v = board.e[i].score;
            fmt_u(sc, v < 0 ? 0u : (unsigned)v);
            surf_draw_text(s, s->w - 140, y, sc, fg, SURF_TRANSPARENT, 2);
            y += 30;
        }
    }

    surf_fill_rect(s, 0, s->h - 36, s->w, 36, rgb(18, 40, 40));
    surf_draw_text(s, 24, s->h - 24, "B(Z): BACK",
                   rgb(110, 190, 150), SURF_TRANSPARENT, 1);
}

/* ──────── Player-picker screen ──────── */

/* List layout: [0..count-1] users, [count] = NEW USER,
 * [count+1] = NOBODY (only while picking player 2). */
static int  pick_stage;    /* 1 = choosing P1, 2 = choosing P2 */
static int  pick_sel;
static int  pending_p1;    /* Stage-1 choice, applied when stage 2 ends */

static int pick_rows(void) {
    return users.count + 1 + (pick_stage == 2 ? 1 : 0);
}

static void draw_users(surface_t* s) {
    surf_clear(s, rgb(10, 12, 34));
    surf_fill_rect(s, 0, 0, s->w, 64, rgb(30, 18, 60));
    surf_fill_rect(s, 0, 64, s->w, 3, rgb(180, 120, 255));
    surf_draw_text(s, 24, 20,
                   pick_stage == 1 ? "WHO IS PLAYER 1?" : "WHO IS PLAYER 2?",
                   rgb(255, 255, 255), SURF_TRANSPARENT, 3);

    int y = 100;
    int rows = pick_rows();
    for (int i = 0; i < rows; i++) {
        const char* label;
        char tag[8] = "";
        if (i < users.count) {
            label = users.names[i];
            if (i == pending_p1 && pick_stage == 2) strcpy(tag, "= P1");
        } else if (i == users.count) {
            label = "+ NEW USER";
        } else {
            label = "NOBODY (1 PLAYER)";
        }

        if (i == pick_sel) {
            surf_fill_rect(s, 24, y - 8, s->w - 48, 40, rgb(60, 30, 110));
            surf_draw_rect(s, 24, y - 8, s->w - 48, 40, rgb(190, 140, 255));
            surf_draw_text(s, 36, y, ">", rgb(255, 220, 80), SURF_TRANSPARENT, 2);
        }
        surf_draw_text(s, 64, y, label,
                       i == pick_sel ? rgb(255, 255, 255) : rgb(160, 150, 200),
                       SURF_TRANSPARENT, 2);
        if (tag[0])
            surf_draw_text(s, s->w - 120, y + 4, tag, rgb(150, 200, 255),
                           SURF_TRANSPARENT, 1);
        y += 44;
    }

    surf_fill_rect(s, 0, s->h - 36, s->w, 36, rgb(30, 18, 60));
    surf_draw_text(s, 24, s->h - 24,
                   "A(X): PICK   SELECT(TAB): DELETE   B(Z): BACK",
                   rgb(170, 140, 220), SURF_TRANSPARENT, 1);
}

static void user_delete(int idx) {
    if (idx < 0 || idx >= users.count) return;
    for (int i = idx; i < users.count - 1; i++)
        strcpy(users.names[i], users.names[i + 1]);
    users.count--;
    /* Fix up active indices */
    if (users.p1 == idx) users.p1 = -1;
    else if (users.p1 > idx) users.p1--;
    if (users.p2 == idx) users.p2 = -1;
    else if (users.p2 > idx) users.p2--;
    if (pending_p1 == idx) pending_p1 = -1;
    else if (pending_p1 > idx) pending_p1--;
    users_save();
    apply_session();
}

/* ──────── On-screen keyboard (new user name) ──────── */

#define KB_COLS 10
#define KB_ROWS 4
static const char kb_grid[KB_ROWS][KB_COLS + 1] = {
    "ABCDEFGHIJ",
    "KLMNOPQRST",
    "UVWXYZ0123",
    "456789-_ .",
};

static char name_buf[SESSION_NAME_LEN];
static int  name_len;
static int  kb_x, kb_y;

static void draw_name(surface_t* s) {
    surf_clear(s, rgb(10, 12, 34));
    surf_fill_rect(s, 0, 0, s->w, 64, rgb(30, 18, 60));
    surf_fill_rect(s, 0, 64, s->w, 3, rgb(180, 120, 255));
    surf_draw_text(s, 24, 20, "NEW USER", rgb(255, 255, 255), SURF_TRANSPARENT, 3);

    /* The name being typed, with a blinking cursor cell */
    int bx = 40, by = 100;
    for (int i = 0; i < SESSION_NAME_LEN - 1; i++) {
        uint32_t border = (i == name_len) ? rgb(255, 220, 80) : rgb(80, 90, 150);
        surf_draw_rect(s, bx + i * 40, by, 34, 42, border);
        if (i < name_len)
            surf_draw_char(s, bx + i * 40 + 9, by + 13, name_buf[i],
                           rgb(255, 255, 255), SURF_TRANSPARENT, 2);
    }

    /* Keyboard grid */
    int gx = 40, gy = 190, cw = 48, ch = 44;
    for (int r = 0; r < KB_ROWS; r++) {
        for (int c = 0; c < KB_COLS; c++) {
            int x = gx + c * cw, y = gy + r * ch;
            if (r == kb_y && c == kb_x) {
                surf_fill_rect(s, x - 4, y - 4, cw - 8, ch - 6, rgb(60, 30, 110));
                surf_draw_rect(s, x - 4, y - 4, cw - 8, ch - 6, rgb(190, 140, 255));
            }
            char ch2 = kb_grid[r][c];
            surf_draw_char(s, x + 8, y + 6, ch2 == ' ' ? '_' : ch2,
                           (r == kb_y && c == kb_x) ? rgb(255, 255, 255)
                                                    : rgb(160, 150, 200),
                           SURF_TRANSPARENT, 2);
        }
    }

    surf_fill_rect(s, 0, s->h - 36, s->w, 36, rgb(30, 18, 60));
    surf_draw_text(s, 24, s->h - 24,
                   "A(X): TYPE   B(Z): ERASE   START(ENTER): DONE",
                   rgb(170, 140, 220), SURF_TRANSPARENT, 1);
}

/* Commit the typed name as a new user; returns its index or -1 */
static int name_commit(void) {
    /* Trim trailing spaces */
    while (name_len > 0 && name_buf[name_len - 1] == ' ') name_len--;
    name_buf[name_len] = '\0';
    if (name_len == 0 || users.count >= MAX_USERS) return -1;
    /* Duplicate name: just pick the existing one */
    for (int i = 0; i < users.count; i++)
        if (strcmp(users.names[i], name_buf) == 0) return i;
    strcpy(users.names[users.count], name_buf);
    users.count++;
    users_save();
    return users.count - 1;
}

/* ──────── Main ──────── */

int main(void) {
    arcade_t a;
    if (arcade_init(&a) != 0) {
        write(1, "launcher: no usable framebuffer\n", 32);
        exit(1);
    }

    scan_games();
    users_load();
    apply_session();          /* Restore last session across power cycles */

    /* Resume on the last-played game */
    int selected = 0;
    int last_idx = -1;
    unsigned int last_active = ticks();   /* For attract-mode idle timer */
    int attract_next = 0;                 /* Rotates through demoable games */
#define ATTRACT_MS 15000                  /* Idle this long -> self-play a demo */
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
        /* A game beamed to this console from the LAN? Launch it; the
         * kernel overlays the sender's live state on its first frame. */
        {
            char beamed[16];
            if (beam_poll(beamed) && beamed[0]) {
                char bpath[80] = "/games/";
                strcpy(bpath + 7, beamed);
                char* bargv[] = { bpath, (char*)0 };
                int bpid = spawn(bpath, bargv);
                if (bpid >= 0) {
                    wait(bpid);
                    a.pad.buttons = 0xFFFF;
                    scan_games();
                }
                draw_home(&a.screen, selected, last_idx, ticks());
                continue;
            }
        }

        /* Any input keeps the console awake (defers attract mode) */
        if (a.pressed || a.pressed2) last_active = ticks();

        if (screen == SCR_HOME) {
            /* Idle at the home screen: demo a game, arcade-cabinet style.
             * Try each game in turn until one has a saved .DEM; the
             * kernel replays it and any button drops back here. */
            if (num_games > 0 && ticks() - last_active > ATTRACT_MS) {
                for (int t = 0; t < num_games; t++) {
                    int gi = (attract_next + t) % num_games;
                    if (attract_op(0, games[gi].name)) {   /* Armed a demo */
                        attract_next = gi + 1;
                        char apath[80] = "/games/";
                        strcpy(apath + 7, games[gi].name);
                        char* aargv[] = { apath, (char*)0 };
                        int apid = spawn(apath, aargv);
                        if (apid >= 0) {
                            wait(apid);
                            a.pad.buttons = 0xFFFF;
                            scan_games();
                        }
                        break;
                    }
                }
                last_active = ticks();     /* Reset whether or not it ran */
                draw_home(&a.screen, selected, last_idx, ticks());
                continue;
            }

            if ((a.pressed & PAD_BTN_DOWN) && num_games > 0) {
                selected = (selected + 1) % num_games;
                sfx_move();
            }
            if ((a.pressed & PAD_BTN_UP) && num_games > 0) {
                selected = (selected + num_games - 1) % num_games;
                sfx_move();
            }
            if (a.pressed & PAD_BTN_Y) {
                sfx_select();
                screen = SCR_USERS;
                pick_stage = 1;
                pick_sel = (users.p1 >= 0) ? users.p1 : 0;
                pending_p1 = users.p1;
            }
            if (a.pressed & PAD_BTN_X) {
                sfx_select();
                board_load();
                screen = SCR_SCORES;
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
                    a.pad.buttons = 0xFFFF;  /* Swallow held buttons */
                    demo_save(games[selected].name);  /* Capture an attract demo */
                    scan_games();
                    last_active = ticks();
                }
            }

            draw_home(&a.screen, selected, last_idx, ticks());
        }

        else if (screen == SCR_USERS) {
            int rows = pick_rows();
            if ((a.pressed & PAD_BTN_DOWN) && rows > 0) {
                pick_sel = (pick_sel + 1) % rows;
                sfx_move();
            }
            if ((a.pressed & PAD_BTN_UP) && rows > 0) {
                pick_sel = (pick_sel + rows - 1) % rows;
                sfx_move();
            }
            if (a.pressed & PAD_BTN_B) {
                sfx_move();
                screen = SCR_HOME;
            }
            if ((a.pressed & PAD_BTN_SELECT) && pick_sel < users.count) {
                sfx_lose();
                user_delete(pick_sel);
                if (pick_sel >= pick_rows()) pick_sel = pick_rows() - 1;
            }
            if (a.pressed & PAD_BTN_A) {
                sfx_select();
                if (pick_sel == users.count) {
                    /* + NEW USER */
                    name_len = 0;
                    kb_x = kb_y = 0;
                    screen = SCR_NAME;
                } else if (pick_stage == 1) {
                    pending_p1 = pick_sel;
                    pick_stage = 2;
                    pick_sel = users.count + 1;   /* Default: NOBODY */
                    if (users.p2 >= 0 && users.p2 != pending_p1)
                        pick_sel = users.p2;
                } else {
                    /* Stage 2 pick: a user or NOBODY */
                    users.p1 = pending_p1;
                    users.p2 = (pick_sel <= users.count - 1 &&
                                pick_sel != pending_p1) ? pick_sel : -1;
                    users_save();
                    apply_session();
                    screen = SCR_HOME;
                }
            }
            draw_users(&a.screen);
        }

        else if (screen == SCR_SCORES) {
            if (a.pressed & (PAD_BTN_B | PAD_BTN_START | PAD_BTN_X)) {
                sfx_move();
                screen = SCR_HOME;
            }
            draw_scores(&a.screen);
        }

        else { /* SCR_NAME */
            if (a.pressed & PAD_BTN_LEFT)  { kb_x = (kb_x + KB_COLS - 1) % KB_COLS; sfx_move(); }
            if (a.pressed & PAD_BTN_RIGHT) { kb_x = (kb_x + 1) % KB_COLS; sfx_move(); }
            if (a.pressed & PAD_BTN_UP)    { kb_y = (kb_y + KB_ROWS - 1) % KB_ROWS; sfx_move(); }
            if (a.pressed & PAD_BTN_DOWN)  { kb_y = (kb_y + 1) % KB_ROWS; sfx_move(); }

            if (a.pressed & PAD_BTN_A) {
                if (name_len < SESSION_NAME_LEN - 1) {
                    name_buf[name_len++] = kb_grid[kb_y][kb_x];
                    sfx_hit();
                }
            }
            if (a.pressed & PAD_BTN_B) {
                if (name_len > 0) { name_len--; sfx_move(); }
                else              { screen = SCR_USERS; }
            }
            if (a.pressed & PAD_BTN_START) {
                int idx = name_commit();
                if (idx >= 0) {
                    sfx_score();
                    if (pick_stage == 1) {
                        pending_p1 = idx;
                        pick_stage = 2;
                        pick_sel = users.count + 1;   /* Default: NOBODY */
                    } else {
                        pick_sel = idx;
                    }
                } else {
                    sfx_lose();
                }
                screen = SCR_USERS;
            }
            draw_name(&a.screen);
        }
    }

    return 0;
}
