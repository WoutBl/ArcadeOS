#ifndef SESSION_H
#define SESSION_H

#include "types.h"

/*
 * ArcadeOS – play session + central highscore board
 *
 * The launcher declares who is playing (SYS_SESSION SET, one or two
 * usernames); games and the REST API can read it back. The kernel
 * watches the live score stream every game already reports
 * (SYS_SCORE) and keeps the best score per (game, player 1) in a
 * scoreboard that persists to HISCORE0.SAV on the game volume — no
 * game code involved. The user LIST itself belongs to the launcher
 * (USERS0.SAV, a normal save file); the kernel only cares about the
 * active session.
 */

#define SESSION_NAME 13          /* Max username: 12 chars + NUL */

/* Set the active players (count 1 or 2; p2 may be NULL/empty) */
void session_set(int count, const char* p1, const char* p2);

/* Read the active players. Buffers must hold SESSION_NAME bytes.
 * Returns the player count (>= 1; defaults to 1 x "GUEST"). */
int session_players(char* p1, char* p2);

/* Live-score hook (called from SYS_SCORE): updates the in-RAM
 * scoreboard when the current game+player beats their best. */
void session_score_report(int score);

/* Load HISCORE0.SAV (call once the FAT32 volume is mounted). */
void session_init(void);

/* Throttled persistence of a dirty scoreboard — idle task. */
void session_idle_flush(void);

#endif /* SESSION_H */
