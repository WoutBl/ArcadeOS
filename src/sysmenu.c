/*
 * ArcadeOS – kernel system menu (see sysmenu.h)
 */

#include "sysmenu.h"
#include "rewind.h"
#include "fb.h"
#include "gamepad.h"
#include "usb.h"
#include "audio.h"
#include "task.h"
#include "scheduler.h"
#include "vga.h"

static volatile int menu_requested = 0;

void sysmenu_request(void) {
    menu_requested = 1;
}

/* ──────── The menu ──────── */

/*
 * The visual save-state picker. Layout:
 *   CONTINUE
 *   [thumb] [thumb] [thumb]     <- captured screens, newest first
 *   [thumb] [thumb] [thumb]
 *   QUIT TO LAUNCHER
 * row = 0 CONTINUE, 1 = the grid (gsel picks within), 2 = QUIT.
 */
#define TH_W REWIND_THUMB_W
#define TH_H REWIND_THUMB_H

static void draw_menu(int row, int gsel, int n_rewind) {
    int cols    = 3;
    int grows   = (n_rewind + cols - 1) / cols;
    int w       = cols * TH_W + (cols - 1) * 10 + 32;
    if (w < 336) w = 336;
    int h       = 96 + 30 + grows * (TH_H + 26) + 30;
    int x       = ((int)fb_width() - w) / 2;
    int y       = ((int)fb_height() - h) / 2;

    fb_overlay_rect(x - 4, y - 4, w + 8, h + 8, 0x7080FF);      /* Border */
    fb_overlay_rect(x, y, w, h, 0x101430);                      /* Panel */
    fb_overlay_text(x + 24, y + 20, "ARCADE OS", 0xFFFFFF, 2);
    fb_overlay_rect(x, y + 48, w, 2, 0x7080FF);

    int ey = y + 62;

    /* CONTINUE */
    if (row == 0) {
        fb_overlay_rect(x + 12, ey - 5, w - 24, 26, 0x2A3C8C);
        fb_overlay_text(x + 20, ey, ">", 0xFFDC50, 2);
    }
    fb_overlay_text(x + 44, ey, "CONTINUE",
                    row == 0 ? 0xFFFFFF : 0x96A0C8, 2);
    ey += 30;

    /* Snapshot grid: pick the exact moment to return to */
    for (int i = 0; i < n_rewind; i++) {
        int cx = x + 16 + (i % cols) * (TH_W + 10);
        int cy = ey + (i / cols) * (TH_H + 26);
        int hot = (row == 1 && i == gsel);

        fb_overlay_rect(cx - 2, cy - 2, TH_W + 4, TH_H + 4,
                        hot ? 0xFFDC50 : 0x39418C);
        const uint32_t* th = rewind_snapshot_thumb(i);
        if (th) fb_overlay_image(cx, cy, TH_W, TH_H, th);

        /* Age label under the shot: "2S" .. "12S" */
        unsigned secs = (rewind_snapshot_age_ms(i) + 500) / 1000;
        char lab[8];
        int n = 0;
        if (secs >= 10) lab[n++] = (char)('0' + (secs / 10) % 10);
        lab[n++] = (char)('0' + secs % 10);
        lab[n++] = 'S';
        lab[n] = '\0';
        fb_overlay_text(cx + TH_W / 2 - n * 4, cy + TH_H + 5, lab,
                        hot ? 0xFFDC50 : 0x7884C8, 1);
    }
    ey += grows * (TH_H + 26);

    /* QUIT */
    if (row == 2) {
        fb_overlay_rect(x + 12, ey - 5, w - 24, 26, 0x2A3C8C);
        fb_overlay_text(x + 20, ey, ">", 0xFFDC50, 2);
    }
    fb_overlay_text(x + 44, ey, "QUIT TO LAUNCHER",
                    row == 2 ? 0xFF9078 : 0x96A0C8, 2);

    fb_overlay_text(x + 24, y + h - 18, "PICK A MOMENT TO JUMP BACK TO",
                    0x5A64A0, 1);
}

/* SYS_EXIT semantics without the game's cooperation */
static void quit_to_launcher(void) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("[MENU] Quit to launcher\n");
    audio_stop_voice(-1);

    current_task->state = TASK_DEAD;
    for (int i = 0; i < num_tasks; i++) {
        if (tasks[i].state == TASK_BLOCKED && tasks[i].wait_pid == current_task->id) {
            tasks[i].state = TASK_READY;
            tasks[i].wait_pid = 0;
        }
    }
    for (;;) {
        schedule();
        asm volatile("sti\nhlt");
    }
}

void sysmenu_on_present(void) {
    if (!menu_requested || !current_task) return;
    menu_requested = 0;
    if (!fb_available()) return;

    /* Never over the console's home screen: QUIT would strand it */
    if (strcmp(current_task->name, "/games/LAUNCHER.ELF") == 0) return;

    audio_stop_voice(-1);                 /* Freeze the soundscape too */
    audio_tone_voice(MIX_VOICES - 1, 700, 60, 140);   /* Open blip */

    int n_rewind = rewind_snapshot_count();
    if (n_rewind > 6) n_rewind = 6;
    int row = 0, gsel = 0;

    uint16_t prev = 0xFFFF;               /* Ignore whatever opened us */
    for (;;) {
        usb_poll();
        pad_state_t p;
        gamepad_get_state(0, &p);
        uint16_t pressed = (uint16_t)(p.buttons & ~prev);
        prev = p.buttons;

        if (pressed & PAD_BTN_DOWN) {
            if (row == 0)      row = n_rewind ? 1 : 2;
            else if (row == 1) {
                if (gsel + 3 < n_rewind) gsel += 3;   /* Next grid row */
                else row = 2;
            } else row = 0;
        }
        if (pressed & PAD_BTN_UP) {
            if (row == 2)      row = n_rewind ? 1 : 0;
            else if (row == 1) {
                if (gsel >= 3) gsel -= 3;
                else row = 0;
            } else row = 2;
        }
        if (row == 1 && (pressed & PAD_BTN_LEFT)  && gsel > 0)            gsel--;
        if (row == 1 && (pressed & PAD_BTN_RIGHT) && gsel < n_rewind - 1) gsel++;

        if (pressed & PAD_BTN_B) break;                          /* = CONTINUE */
        if (pressed & (PAD_BTN_A | PAD_BTN_START)) {
            if (row == 0) break;                                 /* CONTINUE */
            if (row == 1) {                                      /* JUMP */
                rewind_request_restore(gsel);
                break;
            }
            quit_to_launcher();                                  /* No return */
        }

        draw_menu(row, gsel, n_rewind);
        asm volatile("sti\nhlt");        /* Let the PIT/scheduler breathe */
    }

    /* Don't leak menu buttons into the game: wait for full release
     * (the reads above already drained the press-edge latches). */
    for (;;) {
        pad_state_t p;
        gamepad_get_state(0, &p);
        if (p.buttons == 0) break;
        asm volatile("sti\nhlt");
    }
}
