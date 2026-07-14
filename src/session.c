/*
 * ArcadeOS – play session + central highscore board (see session.h)
 */

#include "session.h"
#include "console_abi.h"
#include "task.h"
#include "scheduler.h"
#include "fat32.h"
#include "clock.h"
#include "vga.h"

#define HISCORE_FILE  "HISCORE0.SAV"
#define FLUSH_MS      5000

/* Active session */
static int  player_count = 1;
static char player1[SESSION_NAME] = "GUEST";
static char player2[SESSION_NAME] = "";

/* In-RAM scoreboard, persisted by the idle task when dirty */
static hiscore_file_t board;
static int board_dirty = 0;
static uint32_t last_flush = 0;

static void copy_name(char* dst, const char* src) {
    int i = 0;
    if (src) {
        for (; i < SESSION_NAME - 1 && src[i]; i++) {
            char c = src[i];
            /* Keep it screen- and JSON-safe */
            if (c < 0x20 || c > 0x7E || c == '"' || c == '\\') c = '_';
            dst[i] = c;
        }
    }
    dst[i] = '\0';
}

void session_set(int count, const char* p1, const char* p2) {
    player_count = (count == 2) ? 2 : 1;
    copy_name(player1, (p1 && p1[0]) ? p1 : "GUEST");
    if (player_count == 2)
        copy_name(player2, (p2 && p2[0]) ? p2 : "GUEST 2");
    else
        player2[0] = '\0';
}

int session_players(char* p1, char* p2) {
    if (p1) { memcpy(p1, player1, SESSION_NAME); }
    if (p2) { memcpy(p2, player2, SESSION_NAME); }
    return player_count;
}

/* "/games/PONG.ELF" → "PONG" (also strips a bare "PONG.ELF") */
static void game_of_task(char out[12]) {
    const char* n = current_task ? current_task->name : "";
    const char* base = n;
    for (const char* p = n; *p; p++)
        if (*p == '/') base = p + 1;

    int i = 0;
    for (; i < 11 && base[i] && base[i] != '.'; i++)
        out[i] = base[i];
    out[i] = '\0';
}

void session_score_report(int score) {
    if (score <= 0) return;

    char game[12];
    game_of_task(game);
    if (game[0] == '\0' || strcmp(game, "LAUNCHER") == 0) return;

    /* Find the (game, player1) entry */
    hiscore_entry_t* slot = 0;
    for (uint32_t i = 0; i < board.count && i < HISCORE_MAX; i++) {
        if (strcmp(board.e[i].game, game) == 0 &&
            strcmp(board.e[i].user, player1) == 0) {
            slot = &board.e[i];
            break;
        }
    }

    if (!slot) {
        if (board.count >= HISCORE_MAX) {
            /* Full: evict the lowest score */
            slot = &board.e[0];
            for (uint32_t i = 1; i < HISCORE_MAX; i++)
                if (board.e[i].score < slot->score) slot = &board.e[i];
            if (score <= slot->score) return;
        } else {
            slot = &board.e[board.count++];
        }
        memset(slot, 0, sizeof(*slot));
        strncpy(slot->game, game, sizeof(slot->game) - 1);
        memcpy(slot->user, player1, SESSION_NAME);
        slot->score = score;
        board_dirty = 1;
        return;
    }

    if (score > slot->score) {
        slot->score = score;
        board_dirty = 1;
    }
}

void session_init(void) {
    memset(&board, 0, sizeof(board));
    board.magic = HISCORE_MAGIC;

    hiscore_file_t tmp;
    if (fat32_available() &&
        fat32_load(HISCORE_FILE, (uint8_t*)&tmp, sizeof(tmp)) ==
            (int)sizeof(tmp) &&
        tmp.magic == HISCORE_MAGIC && tmp.count <= HISCORE_MAX) {
        board = tmp;
    }
}

void session_idle_flush(void) {
    if (!board_dirty) return;
    if (!fat32_available()) return;
    if (system_ticks - last_flush < FLUSH_MS) return;

    cli();
    if (fat32_busy()) { sti(); return; }
    fat32_save(HISCORE_FILE, (const uint8_t*)&board, sizeof(board));
    board_dirty = 0;
    last_flush = system_ticks;
    sti();
}
