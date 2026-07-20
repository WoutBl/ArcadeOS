/*
 * ArcadeOS – game beaming (see beam.h)
 *
 * Wire protocol on UDP port 7778 (all little-endian, host is x86):
 *   OFFER   sender→broadcast  {game, npages, vticks, score}
 *   ACCEPT  recv→sender        the game is running and ready for pages
 *   PAGE    sender→recv        {page index, byte offset} + 1 KiB of data
 *   DONE    sender→recv        {npages}  — all pages sent
 *   ACK     recv→sender        got everything, the game lives here now
 *   NACK    recv→sender        incomplete / can't — resend or abort
 *
 * Pages move in 1 KiB chunks (4 per 4 KiB page) so each datagram stays
 * well under the Ethernet MTU. Page i on the sender maps to page i on
 * the receiver: identical ELF → identical writable-page list (same
 * order from paging_collect_user_rw), so an index is all the addressing
 * we need. Chunks are written straight into the receiver's live frames.
 */

#include "beam.h"
#include "paging.h"
#include "task.h"
#include "scheduler.h"
#include "net.h"
#include "rewind.h"
#include "fb.h"
#include "audio.h"
#include "clock.h"
#include "vga.h"
#include "vfs.h"
#include "idt.h"

#define BM_MAGIC   0x4245414Du   /* 'BEAM' */
enum { BM_OFFER = 1, BM_ACCEPT, BM_PAGE, BM_DONE, BM_ACK, BM_NACK };

#define BEAM_CHUNK    1024
#define BEAM_HDR      48                 /* Header bytes before data[] */
#define BEAM_MAX_PAGES 1100              /* App window + stack, worst case */

typedef struct {
    uint32_t magic, type;
    uint32_t npages;     /* OFFER/DONE */
    uint32_t page;       /* PAGE index */
    uint32_t off;        /* PAGE byte offset (0/1024/2048/3072) */
    uint32_t vticks;     /* OFFER: game-visible SYS_TICKS */
    uint32_t score;      /* OFFER: informational */
    uint32_t resv;
    char     game[16];   /* OFFER: ELF basename */
    uint8_t  data[BEAM_CHUNK];
} __attribute__((packed)) beam_pkt_t;

extern registers_t* g_user_regs;   /* current syscall frame */

enum { R_IDLE, R_SEND, R_WANT, R_ACTIVE };
static volatile int role = R_IDLE;

/* Sender */
static user_page_t s_pages[BEAM_MAX_PAGES];
static int         s_np;
static registers_t s_regs;   /* sender game context */
static volatile int      s_accepted, s_ack, s_nack;
static volatile uint32_t s_peer;

/* Receiver */
static char        r_game[16];
static uint32_t    r_peer, r_np, r_vt, r_score;
static uint32_t    r_want_deadline;
static user_page_t r_pages[BEAM_MAX_PAGES];
static int         r_have_pages, r_npages_local;
static uint32_t    r_owner_seen;
static volatile int      r_got_done;
static volatile uint32_t r_done_np;
static uint8_t  r_bits[BEAM_MAX_PAGES * 4 / 8 + 1];  /* chunk-received bitmap */
static volatile uint32_t r_unique;                   /* distinct chunks so far */
static registers_t r_regs;   /* received game context */

/* ──────── Small helpers ──────── */

static const char* basename_of(const char* path) {
    const char* b = path;
    for (const char* q = path; *q; q++)
        if (*q == '/') b = q + 1;
    return b;
}

static void send_ctrl(uint32_t ip, uint32_t type, uint32_t np) {
    static beam_pkt_t p;
    memset(&p, 0, BEAM_HDR);
    p.magic = BM_MAGIC;
    p.type  = type;
    p.npages = np;
    net_beam_send(ip, &p, BEAM_HDR);
}

static void send_offer(const char* base, uint32_t np, uint32_t vt, uint32_t sc) {
    static beam_pkt_t p;
    memset(&p, 0, BEAM_HDR);
    p.magic = BM_MAGIC; p.type = BM_OFFER;
    p.npages = np; p.vticks = vt; p.score = sc;
    for (int i = 0; i < 15 && base[i]; i++) p.game[i] = base[i];
    net_beam_send(0xFFFFFFFFu, &p, BEAM_HDR);   /* Broadcast */
}

/* Centred status card drawn straight on the framebuffer */
static void beam_overlay(const char* title, const char* sub, int pct) {
    if (!fb_available()) return;
    int w = 400, h = 120;
    int x = ((int)fb_width() - w) / 2;
    int y = ((int)fb_height() - h) / 2;
    fb_overlay_rect(x - 4, y - 4, w + 8, h + 8, 0x7080FF);
    fb_overlay_rect(x, y, w, h, 0x101430);
    fb_overlay_text(x + 24, y + 20, title, 0xFFDC50, 2);
    if (sub) fb_overlay_text(x + 24, y + 52, sub, 0xC8D0FF, 1);
    if (pct >= 0) {
        int bw = w - 48;
        fb_overlay_rect(x + 24, y + 80, bw, 16, 0x39418C);
        fb_overlay_rect(x + 24, y + 80, bw * (pct > 100 ? 100 : pct) / 100, 16,
                        0x78DC96);
    }
}

/* ──────── Sender ──────── */

static void stream_pages(uint32_t peer) {
    static beam_pkt_t p;
    memset(&p, 0, sizeof(p));
    p.magic = BM_MAGIC; p.type = BM_PAGE;

    int sent = 0, total = s_np * (4096 / BEAM_CHUNK);
    for (int i = 0; i < s_np; i++) {
        for (uint32_t off = 0; off < 4096; off += BEAM_CHUNK) {
            p.page = (uint32_t)i;
            p.off  = off;
            cr3_borrow_t b = paging_borrow_kernel_cr3();
            memcpy(p.data, (void*)(uintptr_t)(s_pages[i].phys + off), BEAM_CHUNK);
            paging_return_cr3(b);
            net_beam_send(peer, &p, BEAM_HDR + BEAM_CHUNK);

            if ((++sent & 31) == 0) {
                net_poll();                       /* Drain TX + catch NACK */
                beam_overlay("BEAMING GAME", "TRANSFERRING STATE...",
                             total ? sent * 100 / total : 0);
                asm volatile("sti\nhlt");
            }
        }
    }
    /* DONE carries the sender's CPU context so the receiver resumes at
     * the exact RIP/RSP — the game's call depth need not match. */
    static beam_pkt_t d;
    memset(&d, 0, sizeof(d));
    d.magic = BM_MAGIC; d.type = BM_DONE; d.npages = (uint32_t)s_np;
    memcpy(d.data, &s_regs, sizeof(s_regs));
    net_beam_send(peer, &d, BEAM_HDR + sizeof(s_regs));
}

int beam_send_current(void) {
    if (!current_task || net_local_ip() == 0) return 0;

    const char* base = basename_of(current_task->name);
    s_np = paging_collect_user_rw(s_pages, BEAM_MAX_PAGES);
    if (s_np <= 0 || !g_user_regs) return 0;
    s_regs = *g_user_regs;                 /* Freeze this game's CPU state */
    uint32_t vt = rewind_ticks();

    role = R_SEND; s_accepted = 0; s_ack = 0; s_nack = 0; s_peer = 0;

    /* Discovery: broadcast the offer until a console accepts */
    uint32_t deadline = system_ticks + 8000, next = 0;
    while (system_ticks < deadline && !s_accepted) {
        if (system_ticks >= next) {
            send_offer(base, (uint32_t)s_np, vt, 0);
            next = system_ticks + 400;
        }
        net_poll();
        beam_overlay("BEAMING GAME", "LOOKING FOR A CONSOLE ON THE LAN...", -1);
        asm volatile("sti\nhlt");
    }
    if (!s_accepted) { role = R_IDLE; return 0; }

    uint32_t peer = s_peer;

    /* Stream, wait for ACK; on NACK/timeout resend the whole set */
    for (int attempt = 0; attempt < 4 && !s_ack; attempt++) {
        s_nack = 0;
        stream_pages(peer);
        uint32_t until = system_ticks + 2500;
        while (system_ticks < until && !s_ack && !s_nack) {
            net_poll();
            beam_overlay("BEAMING GAME", "CONFIRMING...", 100);
            asm volatile("sti\nhlt");
        }
    }

    if (s_ack) {
        beam_overlay("GAME BEAMED", "IT LIVES ON THE OTHER CONSOLE NOW", 100);
        audio_tone_voice(MIX_VOICES - 1, 1400, 120, 160);
        uint32_t t = system_ticks + 900;
        while (system_ticks < t) asm volatile("sti\nhlt");
    }
    role = R_IDLE;
    return s_ack ? 1 : 0;
}

/* ──────── Receiver ──────── */

void beam_input(uint32_t src, const void* data, uint32_t len) {
    if (len < BEAM_HDR) return;
    const beam_pkt_t* p = (const beam_pkt_t*)data;
    if (p->magic != BM_MAGIC) return;

    switch (p->type) {
    case BM_OFFER:
        if (role != R_IDLE) break;
        {
            /* Sanitise the name and require the ELF to exist locally */
            char nm[16];
            int i;
            for (i = 0; i < 15 && p->game[i]; i++) {
                char c = p->game[i];
                nm[i] = (c >= 32 && c < 127) ? c : '_';
            }
            nm[i] = '\0';
            char path[32] = "/games/";
            int j = 7;
            for (int k = 0; nm[k] && j < 31; k++) path[j++] = nm[k];
            path[j] = '\0';
            vfs_node_t* n = vfs_open(path, 0);
            if (!n || !(n->flags & VFS_FLAG_FILE)) break;

            for (i = 0; i < 16; i++) r_game[i] = nm[i];
            r_peer = src; r_np = p->npages; r_vt = p->vticks; r_score = p->score;
            r_have_pages = 0; r_owner_seen = 0;
            r_want_deadline = system_ticks + 9000;
            role = R_WANT;
            terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
            terminal_writestring("[BEAM] Incoming game: ");
            terminal_writestring(r_game);
            terminal_writestring("\n");
        }
        break;

    case BM_ACCEPT:
        if (role == R_SEND) { s_peer = src; s_accepted = 1; }
        break;

    case BM_PAGE:
        if (role == R_ACTIVE && r_have_pages &&
            p->page < (uint32_t)r_npages_local && p->off + BEAM_CHUNK <= 4096) {
            cr3_borrow_t b = paging_borrow_kernel_cr3();
            memcpy((void*)(uintptr_t)(r_pages[p->page].phys + p->off),
                   p->data, BEAM_CHUNK);
            paging_return_cr3(b);
            uint32_t bit = p->page * 4 + p->off / BEAM_CHUNK;
            if (!(r_bits[bit >> 3] & (1 << (bit & 7)))) {
                r_bits[bit >> 3] |= (uint8_t)(1 << (bit & 7));
                r_unique++;      /* Count each chunk once; resends fill holes */
            }
        }
        break;

    case BM_DONE:
        if (role == R_ACTIVE) {
            r_done_np = p->npages;
            if (len >= BEAM_HDR + sizeof(r_regs))
                memcpy(&r_regs, p->data, sizeof(r_regs));
            r_got_done = 1;
        }
        break;

    case BM_ACK:  if (role == R_SEND) s_ack = 1; break;
    case BM_NACK: if (role == R_SEND) s_nack = 1; break;
    }
}

const char* beam_pending_game(void) {
    if (role == R_WANT) {
        if (system_ticks > r_want_deadline) { role = R_IDLE; return 0; }
        return r_game;
    }
    return 0;
}

void beam_on_present(void) {
    if (role != R_WANT || !current_task) return;
    if (strcmp(basename_of(current_task->name), r_game) != 0) return;
    if (current_task->id == r_owner_seen) return;   /* Once per game */
    r_owner_seen = current_task->id;

    /* This process's writable pages must line up with the sender's */
    r_npages_local = paging_collect_user_rw(r_pages, BEAM_MAX_PAGES);
    if (r_npages_local != (int)r_np) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("[BEAM] page-count mismatch, aborting\n");
        send_ctrl(r_peer, BM_NACK, 0);
        role = R_IDLE;
        return;
    }
    r_have_pages = 1;
    role = R_ACTIVE;
    r_unique = 0;
    memset(r_bits, 0, sizeof(r_bits));   /* Bitmap persists across resends */

    send_ctrl(r_peer, BM_ACCEPT, 0);

    uint32_t need = r_np * (4096 / BEAM_CHUNK);
    int ok = 0;
    for (int attempt = 0; attempt < 4 && !ok; attempt++) {
        r_got_done = 0;
        uint32_t deadline = system_ticks + 8000;
        while (system_ticks < deadline && !r_got_done) {
            net_poll();
            beam_overlay("RECEIVING BEAMED GAME", "HOLD ON...",
                         (int)(r_unique * 100 / (need ? need : 1)));
            asm volatile("sti\nhlt");
        }
        if (r_got_done && r_unique >= need) { ok = 1; break; }
        if (attempt < 3) send_ctrl(r_peer, BM_NACK, 0);   /* Resend the holes */
    }

    if (ok) {
        /* Resume at the sender's exact execution point: overwrite this
         * syscall's user frame with the sender's callee-saved regs +
         * RIP/RSP/RFLAGS. Combined with the overlaid memory, the game
         * continues as if it had never left the other console — no
         * dependence on matching call depth. */
        if (g_user_regs) {
            registers_t* r = g_user_regs;
            r->r15 = r_regs.r15; r->r14 = r_regs.r14;
            r->r13 = r_regs.r13; r->r12 = r_regs.r12;
            r->ebp = r_regs.ebp; r->ebx = r_regs.ebx;
            r->eip = r_regs.eip; r->eflags = r_regs.eflags;
            r->useresp = r_regs.useresp;
        }
        rewind_adopt_ticks(r_vt);          /* SYS_TICKS continues seamlessly */
        send_ctrl(r_peer, BM_ACK, 0);
        audio_tone_voice(MIX_VOICES - 1, 1200, 120, 160);
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
        terminal_writestring("[BEAM] Game received - resuming\n");
    } else {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("[BEAM] Transfer incomplete\n");
    }
    r_have_pages = 0;
    role = R_IDLE;
    /* Either way the game continues: on success from the beamed state,
     * on failure from its own fresh start. */
}
