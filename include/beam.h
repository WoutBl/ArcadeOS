#ifndef BEAM_H
#define BEAM_H

#include "types.h"

/*
 * ArcadeOS – game beaming (live process migration over the LAN)
 *
 * Pause a running game, stream its writable pages to another console
 * over UDP, and let the game CONTINUE there — same frame, same score.
 * The trick is the same determinism the rewind engine exploits: two
 * consoles running the identical ELF have bit-identical address-space
 * *structure* (same text, same page layout, same stack shape at every
 * SYS_GFX_PRESENT boundary). Only the writable page *contents* diverge
 * with play, so transferring them turns one console's game into the
 * other's — no CPU context to move, because the stack pointer is a
 * deterministic function of the code path and already matches.
 *
 * Roles (a console is one at a time):
 *   Sender   – the system menu's BEAM entry runs beam_send_current().
 *   Receiver – idle in the launcher; beam_input() catches the offer,
 *              the launcher spawns the game (beam_pending_game()), and
 *              beam_on_present() overlays the state at its first frame.
 */

#define BEAM_PORT 7778

/* Sender: stream the currently-running game to a console on the LAN.
 * Blocks running the whole handshake + transfer with an on-screen
 * status overlay. Returns 1 on success (the game now lives on the peer
 * — the caller should quit the local copy), 0 on timeout/failure. */
int beam_send_current(void);

/* Inbound beam datagram, dispatched from net.c's UDP handler. */
void beam_input(uint32_t src_ip, const void* data, uint32_t len);

/* Launcher poll (SYS_BEAM_POLL): basename of a game a beam wants
 * launched (e.g. "PONG.ELF"), or NULL. The launcher spawns it like a
 * normal pick; the kernel overlays the beamed state on its first
 * present. */
const char* beam_pending_game(void);

/* Present hook (SYS_GFX_PRESENT): when the game a beam is waiting for
 * reaches its first frame, receive the state here (blocks). */
void beam_on_present(void);

#endif /* BEAM_H */
