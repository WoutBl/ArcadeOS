/*
 * ArcadeOS – kernel system menu (see sysmenu.h)
 */

#include "sysmenu.h"
#include "rewind.h"
#include "fb.h"
#include "font8x8.h"
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

/* ──────── Direct framebuffer drawing (displayed page) ──────── */

static void px_rect(int x, int y, int w, int h, uint32_t c) {
    uint32_t* fb    = fb_ptr();
    uint32_t  pitch = fb_pitch() / 4;
    for (int r = 0; r < h; r++)
        for (int cx = 0; cx < w; cx++)
            fb[(uint32_t)(y + r) * pitch + (uint32_t)(x + cx)] = c;
}

static void px_text(int x, int y, const char* s, uint32_t c, int scale) {
    uint32_t* fb    = fb_ptr();
    uint32_t  pitch = fb_pitch() / 4;
    for (int i = 0; s[i]; i++) {
        const uint8_t* g = font8x8_basic[(uint8_t)s[i] & 0x7F];
        for (int r = 0; r < 8; r++)
            for (int b = 0; b < 8; b++) {
                if (!(g[r] & (1 << b))) continue;
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++)
                        fb[(uint32_t)(y + r * scale + sy) * pitch
                           + (uint32_t)(x + i * 8 * scale + b * scale + sx)] = c;
            }
    }
}

/* ──────── The menu ──────── */

static void draw_menu(int sel, int n_rewind) {
    int w = 336, entries = 2 + n_rewind;
    int h = 96 + entries * 30;
    int x = ((int)fb_width() - w) / 2;
    int y = ((int)fb_height() - h) / 2;

    px_rect(x - 4, y - 4, w + 8, h + 8, 0x7080FF);      /* Border */
    px_rect(x, y, w, h, 0x101430);                      /* Panel */
    px_text(x + 24, y + 20, "ARCADE OS", 0xFFFFFF, 2);
    px_rect(x, y + 48, w, 2, 0x7080FF);

    int ey = y + 64;
    for (int i = 0; i < entries; i++) {
        char label[28];
        if (i == 0) {
            char* p = label;
            const char* t = "CONTINUE";
            while (*t) *p++ = *t++;
            *p = '\0';
        } else if (i <= n_rewind) {
            /* "REWIND 12S AGO" */
            unsigned secs = (rewind_snapshot_age_ms(i - 1) + 500) / 1000;
            char* p = label;
            const char* t = "REWIND ";
            while (*t) *p++ = *t++;
            if (secs >= 100) secs = 99;
            if (secs >= 10) *p++ = (char)('0' + secs / 10);
            *p++ = (char)('0' + secs % 10);
            t = "S AGO";
            while (*t) *p++ = *t++;
            *p = '\0';
        } else {
            char* p = label;
            const char* t = "QUIT TO LAUNCHER";
            while (*t) *p++ = *t++;
            *p = '\0';
        }

        if (i == sel) {
            px_rect(x + 12, ey - 6, w - 24, 28, 0x2A3C8C);
            px_text(x + 20, ey, ">", 0xFFDC50, 2);
        }
        px_text(x + 44, ey, label,
                i == sel ? 0xFFFFFF : (i > n_rewind ? 0xFF9078 : 0x96A0C8), 2);
        ey += 30;
    }

    px_text(x + 24, y + h - 20, "SELECT+START OPENS THIS MENU", 0x5A64A0, 1);
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
    if (n_rewind > 4) n_rewind = 4;       /* Keep the panel compact */
    int entries = 2 + n_rewind;
    int sel = 0;

    uint16_t prev = 0xFFFF;               /* Ignore whatever opened us */
    for (;;) {
        usb_poll();
        pad_state_t p;
        gamepad_get_state(0, &p);
        uint16_t pressed = (uint16_t)(p.buttons & ~prev);
        prev = p.buttons;

        if (pressed & PAD_BTN_DOWN) { sel = (sel + 1) % entries; }
        if (pressed & PAD_BTN_UP)   { sel = (sel + entries - 1) % entries; }
        if (pressed & PAD_BTN_B)    break;                       /* = CONTINUE */
        if (pressed & (PAD_BTN_A | PAD_BTN_START)) {
            if (sel == 0) break;                                 /* CONTINUE */
            if (sel <= n_rewind) {                               /* RESTORE */
                rewind_request_restore(sel - 1);
                break;
            }
            quit_to_launcher();                                  /* No return */
        }

        draw_menu(sel, n_rewind);
        asm volatile("sti\nhlt");         /* Let the PIT/scheduler breathe */
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
