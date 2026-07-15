#ifndef SYSMENU_H
#define SYSMENU_H

#include "types.h"

/*
 * ArcadeOS – kernel system menu (universal pause)
 *
 * SELECT+START (Tab+Enter on the keyboard) in any game freezes it and
 * opens a kernel-drawn overlay — zero cooperation from the game, like
 * the rewind itself. The game is parked inside its own SYS_GFX_PRESENT
 * while the kernel polls the pad and draws straight onto the displayed
 * framebuffer page. Entries: CONTINUE, one line per rewind snapshot
 * (with its age — picking one restores it), and QUIT TO LAUNCHER.
 *
 * The launcher is exempt: quitting the console's home screen from a
 * system menu would leave nothing running.
 */

/* Arm the menu (called on the chord edge from the pad filter). */
void sysmenu_request(void);

/* Hook for SYS_GFX_PRESENT, after the blit: runs the whole menu loop
 * synchronously when armed, then returns to the frozen game. */
void sysmenu_on_present(void);

#endif /* SYSMENU_H */
