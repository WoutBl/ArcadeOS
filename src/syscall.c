/*
 * ArcadeOS – System Call Interface (int 0x80)
 *
 * Syscall numbers are passed in EAX:
 *   0 = sys_exit   (halt the user task)
 *   1 = sys_write  (print string; EBX = pointer to null-terminated string)
 *   2 = sys_yield  (voluntarily yield the CPU)
 */

#include "syscall.h"
#include "idt.h"
#include "vga.h"
#include "scheduler.h"
#include "keyboard.h"
#include "loader.h"
#include "clock.h"
#include "vfs.h"
#include "fb.h"
#include "console_gfx.h"
#include "gamepad.h"
#include "usb.h"
#include "console_abi.h"
#include "fat32.h"
#include "audio.h"
#include "net.h"
#include "paging.h"
#include "sysmenu.h"
#include "rewind.h"
#include "session.h"

/*
 * ──────── User pointer validation ────────
 *
 * Every pointer a game hands to a syscall is untrusted. These wrappers
 * walk the calling process's page tables (paging_user_access_ok) and
 * reject anything not mapped PRESENT|USER — including kernel identity-
 * map addresses, which live inside the user PML4 but are supervisor-
 * only. A syscall that gets a bad pointer returns -1 instead of
 * reading/writing kernel memory on the game's behalf.
 */
#define USER_STR_MAX  256      /* Paths, filenames */
#define USER_TEXT_MAX 4096     /* Free-text buffers (legacy strlen write) */

/* Pointer the kernel will READ len bytes through */
static int urd(const void* p, uint64_t len) {
    return p && paging_user_access_ok((uint64_t)(uintptr_t)p, len, 0);
}

/* Pointer the kernel will WRITE len bytes through */
static int uwr(const void* p, uint64_t len) {
    return p && paging_user_access_ok((uint64_t)(uintptr_t)p, len, 1);
}

/* NUL-terminated user string (path/filename length cap) */
static int ustr(const char* s) {
    return s && paging_user_str_ok(s, USER_STR_MAX) >= 0;
}

/* ──────── The global TSS ──────── */
tss_t kernel_tss;

/* ──────── TSS initialization ──────── */
void tss_init(uint64_t kernel_stack_top) {
    memset(&kernel_tss, 0, sizeof(tss_t));

    /* The 64-bit TSS carries no segment registers – only the ring-0
     * stack pointer loaded on a Ring 3 → 0 transition. */
    kernel_tss.rsp0 = kernel_stack_top;

    /* I/O map base: point past the end of the TSS to disable I/O bitmap */
    kernel_tss.iomap_base = sizeof(tss_t);

    /* Load the TSS into the Task Register */
    tss_flush();

    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("[TSS] Loaded (rsp0=0x");
    terminal_writehex(kernel_stack_top);
    terminal_writestring(")\n");
}

void tss_set_kernel_stack(uint64_t stack_top) {
    kernel_tss.rsp0 = stack_top;
}

/* ──────── Syscall dispatcher (called from isr_handler via int 0x80) ──────── */
static void syscall_handler(registers_t* regs) {
    switch (regs->eax) {
        case SYS_EXIT:
            /* Halt the current user task */
            terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
            terminal_writestring("[SYSCALL] sys_exit called\n");
            
            /* Mark task as dead, yield, and wake up any waiters */
            if (current_task != NULL) {
                current_task->state = TASK_DEAD;

                /* Wake up any tasks waiting on this task to exit */
                for (int i = 0; i < num_tasks; i++) {
                    if (tasks[i].state == TASK_BLOCKED && tasks[i].wait_pid == current_task->id) {
                        tasks[i].state = TASK_READY;
                        tasks[i].wait_pid = 0;
                    }
                }
            }
            
            /* Give up the CPU forever without masking hardware interrupts */
            for (;;) {
                schedule();
                asm volatile("sti\nhlt");
            }
            break;

        case SYS_WRITE: {
            /*
             * EBX = fd, ECX = buf pointer, EDX = len
             * Legacy libc passes len=0 meaning "use strlen"; we handle that.
             */
            int          fd  = (int)regs->ebx;
            const char*  str = (const char*)regs->ecx;
            uint32_t     len = (uint32_t)regs->edx;

            if (!str) { regs->eax = (uint32_t)-1; break; }
            if (len == 0) {
                /* Legacy "use strlen" — validate while measuring */
                long l = paging_user_str_ok(str, USER_TEXT_MAX);
                if (l < 0) { regs->eax = (uint32_t)-1; break; }
                len = (uint32_t)l;
            } else if (!urd(str, len)) {
                regs->eax = (uint32_t)-1; break;
            }

            /* Route through VFS if the task has a node for this fd */
            if (current_task && fd >= 0 && fd < MAX_FD && current_task->fds[fd]) {
                regs->eax = (uint32_t)vfs_write(current_task->fds[fd], 0, len,
                                                 (const uint8_t*)str);
            } else {
                /* Fallback: write directly to VGA terminal */
                terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
                terminal_writestring(str);
                regs->eax = len;
            }
            break;
        }

        case SYS_YIELD:
            /* Voluntarily yield the CPU */
            schedule();
            regs->eax = 0;
            break;

        case SYS_READ: {
            /* fd = EBX, buf = ECX, count = EDX */
            int    fd    = (int)regs->ebx;
            char*  buf   = (char*)regs->ecx;
            size_t count = (size_t)regs->edx;

            if (!uwr(buf, count) || count == 0) { regs->eax = (uint32_t)-1; break; }

            /* Route through VFS if the task has a node for this fd */
            if (current_task && fd >= 0 && fd < MAX_FD && current_task->fds[fd]) {
                regs->eax = (uint32_t)vfs_read(current_task->fds[fd], 0,
                                               (uint32_t)count, (uint8_t*)buf);
            } else if (fd == 0) {
                /* Fallback: read from keyboard directly (stdin) */
                size_t bytes_read = 0;
                while (bytes_read < count) {
                    char c = keyboard_read_blocking();
                    if (c == '\b') { if (bytes_read > 0) bytes_read--; continue; }
                    buf[bytes_read++] = c;
                    if (c == '\n') break;
                }
                regs->eax = (uint32_t)bytes_read;
            } else {
                regs->eax = (uint32_t)-1;
            }
            break;
        }

        case SYS_SPAWN: {
            const char* path = (const char*)regs->ebx;
            char* const* argv = (char* const*)regs->ecx;
            if (!ustr(path)) {
                regs->eax = (uint32_t)-1;
                break;
            }
            /* argv is a user array of user string pointers — validate
             * every element before launch_user_app copies them. */
            if (argv) {
                int bad_argv = 0;
                for (int n = 0; ; n++) {
                    if (n >= 32 || !urd(&argv[n], sizeof(char*))) { bad_argv = 1; break; }
                    if (!argv[n]) break;
                    if (!ustr(argv[n])) { bad_argv = 1; break; }
                }
                if (bad_argv) { regs->eax = (uint32_t)-1; break; }
            }

            /*
             * Inherit FD table: copy the calling task's fds[] into the new
             * task before it ever runs.  The child inherits pipe ends,
             * redirected stdin/stdout, etc.
             */
            int pid = launch_user_app(path, argv);

            /* Find the child task by PID and copy the FD table */
            if (pid >= 0 && current_task) {
                for (int i = 0; i < num_tasks; i++) {
                    if ((int)tasks[i].id == pid) {
                        /* Inherit STDIN, STDOUT, STDERR. Simulate O_CLOEXEC for all others. */
                        tasks[i].fds[0] = current_task->fds[0];
                        tasks[i].fds[1] = current_task->fds[1];
                        tasks[i].fds[2] = current_task->fds[2];
                        for (int j = 3; j < MAX_FD; j++) {
                            tasks[i].fds[j] = (vfs_node_t*)0;
                        }
                        
                        /* Reset signals for the new process so it doesn't inherit the shell's SIG_IGN! */
                        tasks[i].pending_signals = 0;
                        tasks[i].ignored_signals = 0;
                        break;
                    }
                }
            }

            regs->eax = pid;
            break;
        }

        case SYS_WAIT: {
            int pid = regs->ebx;
            if (current_task != NULL) {
                current_task->state = TASK_BLOCKED;
                current_task->wait_pid = pid;
                schedule();
            }
            regs->eax = 0;
            break;
        }











        case SYS_OPEN: {
            /*
             * EBX = path string, ECX = flags
             * Opens a file/device through the VFS and stores it in the
             * calling process's FD table.  Returns the FD index, or -1.
             */
            const char* path  = (const char*)regs->ebx;
            uint32_t    flags = (uint32_t)regs->ecx;

            if (!ustr(path) || !current_task) { regs->eax = (uint32_t)-1; break; }

            vfs_node_t* node = vfs_open(path, flags);
            if (!node) { regs->eax = (uint32_t)-1; break; }

            /* Find a free FD slot (skip 0/1/2 which are pre-wired) */
            int fd = -1;
            for (int i = 3; i < MAX_FD; i++) {
                if (current_task->fds[i] == (vfs_node_t*)0) {
                    current_task->fds[i] = node;
                    fd = i;
                    break;
                }
            }
            regs->eax = (fd >= 0) ? (uint32_t)fd : (uint32_t)-1;
            break;
        }

        case SYS_CLOSE: {
            /* EBX = fd.  Calls node->close() and clears the FD slot. */
            int fd = (int)regs->ebx;
            if (!current_task || fd < 0 || fd >= MAX_FD) { regs->eax = (uint32_t)-1; break; }

            vfs_node_t* node = current_task->fds[fd];
            if (node) {
                vfs_close(node);
                current_task->fds[fd] = (vfs_node_t*)0;
            }
            regs->eax = 0;
            break;
        }




        case SYS_GFX_INFO: {
            /* EBX = gfx_info_t* – report the framebuffer geometry so the
             * game can allocate a matching pixel buffer. */
            gfx_info_t* info = (gfx_info_t*)regs->ebx;
            if (!uwr(info, sizeof(gfx_info_t)) || !fb_available()) { regs->eax = (uint32_t)-1; break; }

            info->width  = fb_width();
            info->height = fb_height();
            info->pitch  = fb_width() * 4;   /* User buffers are tightly packed */
            info->bpp    = 32;
            regs->eax = 0;
            break;
        }

        case SYS_GFX_PRESENT: {
            /*
             * EBX = pointer to a width*height buffer of 0x00RRGGBB pixels
             * in user space. The kernel blits it to the framebuffer in one
             * call – the user-space half of double buffering.
             */
            uint64_t ptr = regs->ebx;
            if (!fb_available()) { regs->eax = (uint32_t)-1; break; }

            uint32_t bytes = fb_width() * fb_height() * 4;
            /* Whole buffer must be mapped user-readable */
            if (!paging_user_access_ok(ptr, bytes, 0)) {
                regs->eax = (uint32_t)-1;
                break;
            }
            /* Universal rewind: snapshot/restore at the frame boundary.
             * Runs BEFORE the blit so a rewound frame is what appears. */
            rewind_on_present();
            if (rewind_should_blit())
                gfx_present_buffer((const uint32_t*)(uintptr_t)ptr);
            rewind_post_blit();    /* Scrub hold-loop (may block) */
            if (!rewind_busy())
                sysmenu_on_present();  /* SELECT+START menu (may block) */
            regs->eax = 0;
            break;
        }

        case SYS_PAD_READ: {
            /* EBX = pad index, ECX = pad_state_t* out */
            int          index = (int)regs->ebx;
            pad_state_t* out   = (pad_state_t*)regs->ecx;
            if (!uwr(out, sizeof(pad_state_t))) { regs->eax = (uint32_t)-1; break; }

            usb_poll();                    /* Hot-plug + future HID reports */
            gamepad_get_state(index, out);
            rewind_filter_pad(index, out); /* System combos, hidden from games */
            rewind_feed_pad(index, out);   /* Replay feeds the log back */
            regs->eax = 0;
            break;
        }

        case SYS_TICKS:
            /* Milliseconds since boot — virtual-clock shifted after a
             * rewind, logged values during a replay */
            regs->eax = rewind_ticks();
            break;

        case SYS_MSLEEP: {
            /* EBX = ms. Sleep with interrupts enabled so the PIT keeps
             * ticking and other tasks get scheduled. */
            /* Pure tick-wait: hlt wakes on the next PIT interrupt (1 ms).
             * Other tasks still run – scheduler_tick preempts us. */
            if (rewind_replaying()) { regs->eax = 0; break; }  /* Fast-forward */
            uint32_t target = system_ticks + regs->ebx;
            while (system_ticks < target)
                asm volatile("sti\nhlt");
            regs->eax = 0;
            break;
        }

        case SYS_READDIR: {
            /*
             * EBX = absolute directory path, ECX = entry index,
             * EDX = dirent_info_t* out.
             * Returns 0 on success, -1 when the index is past the end.
             * The launcher uses this to list /games.
             */
            const char*    path  = (const char*)regs->ebx;
            uint32_t       index = regs->ecx;
            dirent_info_t* out   = (dirent_info_t*)regs->edx;
            if (!ustr(path) || !uwr(out, sizeof(dirent_info_t))) { regs->eax = (uint32_t)-1; break; }

            vfs_node_t* dir = vfs_open(path, 0);
            if (!dir || !(dir->flags & VFS_FLAG_DIR)) { regs->eax = (uint32_t)-1; break; }

            vfs_dirent_t* de = vfs_readdir(dir, index);
            if (!de) { regs->eax = (uint32_t)-1; break; }

            strncpy(out->name, de->name, sizeof(out->name) - 1);
            out->name[sizeof(out->name) - 1] = '\0';
            out->flags = de->flags;

            /* Look the entry up to report its size */
            out->size = 0;
            vfs_node_t* child = vfs_finddir(dir, de->name);
            if (child) out->size = child->length;

            regs->eax = 0;
            break;
        }

        case SYS_SAVE: {
            /*
             * EBX = bare 8.3 filename, ECX = data, EDX = length.
             * Whole-file save to the game volume root ("memory card"
             * semantics). Slashes are rejected: saves always live on
             * the FAT32 volume, games cannot address other mounts.
             */
            const char*    name = (const char*)regs->ebx;
            const uint8_t* buf  = (const uint8_t*)regs->ecx;
            uint32_t       len  = regs->edx;

            if (!ustr(name) || len > 64 * 1024 || !urd(buf, len)) { regs->eax = (uint32_t)-1; break; }
            if (rewind_replaying()) { regs->eax = 0; break; }  /* Already saved once */
            int bad = 0;
            for (int i = 0; name[i]; i++)
                if (name[i] == '/') { bad = 1; break; }
            if (bad) { regs->eax = (uint32_t)-1; break; }

            regs->eax = (uint32_t)fat32_save(name, buf, len);
            if ((int)regs->eax == 0) {
                terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
                terminal_writestring("[SAVE] Wrote ");
                terminal_writestring(name);
                terminal_writestring("\n");
            }
            break;
        }

        case SYS_LOAD: {
            /* EBX = filename, ECX = out buffer, EDX = max length.
             * Returns bytes read, or -1 if the file doesn't exist. */
            const char* name   = (const char*)regs->ebx;
            uint8_t*    buf    = (uint8_t*)regs->ecx;
            uint32_t    maxlen = regs->edx;

            if (!ustr(name) || !uwr(buf, maxlen)) { regs->eax = (uint32_t)-1; break; }

            regs->eax = (uint32_t)fat32_load(name, buf, maxlen);
            break;
        }

        case SYS_SOUND: {
            /* EBX = frequency in Hz (0 stops playback), ECX = duration ms */
            if (rewind_replaying()) { regs->eax = 0; break; }
            audio_tone((uint32_t)regs->ebx, (uint32_t)regs->ecx);
            regs->eax = 0;
            break;
        }

        case SYS_SOUND_EX: {
            /* EBX = sound_req_t*. Tones and PCM clips on mixer voices.
             * PCM data is validated then COPIED kernel-side, so the
             * game's buffer can be reused immediately. */
            if (rewind_replaying()) { regs->eax = 0; break; }
            const sound_req_t* rq = (const sound_req_t*)regs->ebx;
            if (!urd(rq, sizeof(sound_req_t)) || rq->voice >= SOUND_VOICES) {
                regs->eax = (uint32_t)-1; break;
            }
            uint8_t vol = (rq->vol > 255) ? 255 : (uint8_t)rq->vol;

            switch (rq->op) {
                case SOUND_OP_STOP:
                    audio_stop_voice((int)rq->voice);
                    regs->eax = 0;
                    break;
                case SOUND_OP_SQUARE:
                    regs->eax = (uint32_t)audio_tone_voice((int)rq->voice,
                                    rq->freq_hz, rq->dur_ms, vol);
                    break;
                case SOUND_OP_PCM: {
                    const int16_t* smp = (const int16_t*)(uintptr_t)rq->sample_ptr;
                    if (rq->sample_count == 0 ||
                        rq->sample_count > SOUND_PCM_MAX ||
                        !urd(smp, (uint64_t)rq->sample_count * 2)) {
                        regs->eax = (uint32_t)-1; break;
                    }
                    regs->eax = (uint32_t)audio_pcm_play((int)rq->voice, smp,
                                    rq->sample_count, rq->sample_rate, vol);
                    break;
                }
                default:
                    regs->eax = (uint32_t)-1;
                    break;
            }
            break;
        }

        case SYS_NET: {
            /* EBX = net_req_t*. The game's UDP socket (netplay). */
            net_req_t* rq = (net_req_t*)regs->ebx;
            if (!uwr(rq, sizeof(net_req_t))) { regs->eax = (uint32_t)-1; break; }

            switch (rq->op) {
                case NET_OP_INFO:
                    regs->eax = net_local_ip();
                    break;
                case NET_OP_BIND:
                    regs->eax = (uint32_t)net_udp_bind((uint16_t)rq->port);
                    break;
                case NET_OP_SEND: {
                    const void* p = (const void*)(uintptr_t)rq->buf;
                    if (rq->len > NET_MSG_MAX || !urd(p, rq->len)) {
                        regs->eax = (uint32_t)-1; break;
                    }
                    regs->eax = (uint32_t)net_udp_send(rq->ip,
                                    (uint16_t)rq->port, p, rq->len);
                    break;
                }
                case NET_OP_RECV: {
                    void* p = (void*)(uintptr_t)rq->buf;
                    if (rq->len > NET_MSG_MAX || !uwr(p, rq->len)) {
                        regs->eax = (uint32_t)-1; break;
                    }
                    net_poll();          /* Fresh datagrams before we look */
                    uint32_t sip; uint16_t sport;
                    int n = net_udp_recv(p, rq->len, &sip, &sport);
                    if (n >= 0) {
                        rq->ip   = sip;
                        rq->port = sport;
                        rq->len  = (uint32_t)n;
                    }
                    regs->eax = (uint32_t)n;
                    break;
                }
                default:
                    regs->eax = (uint32_t)-1;
                    break;
            }
            break;
        }

        case SYS_SCORE:
            if (rewind_replaying()) { regs->eax = 0; break; }
        {
            /* Live score report for the REST API (/api/status) and the
             * central highscore board */
            if (current_task) {
                score_report((int)regs->ebx, current_task->id, current_task->name);
                session_score_report((int)regs->ebx);
            }
            regs->eax = 0;
            break;
        }

        case SYS_SESSION: {
            /* EBX = session_req_t*. SET declares the active players
             * (launcher); GET reads them back (games, for name tags). */
            session_req_t* rq = (session_req_t*)regs->ebx;
            if (!uwr(rq, sizeof(session_req_t))) { regs->eax = (uint32_t)-1; break; }

            if (rq->op == SESSION_OP_SET) {
                rq->p1[SESSION_NAME_LEN - 1] = '\0';
                rq->p2[SESSION_NAME_LEN - 1] = '\0';
                session_set((int)rq->count, rq->p1, rq->p2);
                regs->eax = 0;
            } else if (rq->op == SESSION_OP_GET) {
                rq->count = (uint32_t)session_players(rq->p1, rq->p2);
                regs->eax = 0;
            } else {
                regs->eax = (uint32_t)-1;
            }
            break;
        }

        default:
            /* Unknown syscall */
            terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
            terminal_writestring("[SYSCALL] Unknown syscall: ");
            terminal_writedec(regs->eax);
            terminal_writestring("\n");
            regs->eax = (uint32_t)-1;  /* Return -1 = error */
            break;
    }
}

/* ──────── Initialize the syscall interface ──────── */
void syscall_init(void) {
    /*
     * Wire int 0x80 into the IDT.
     * Flags = 0xEE: Present (0x80) | DPL 3 (0x60) | 32-bit interrupt gate (0x0E)
     * DPL=3 is critical so ring 3 code is allowed to trigger int 0x80.
     */
    idt_set_gate(128, (uint64_t)isr128, GDT_KERNEL_CODE, 0xEE);

    /* Register the C handler */
    register_interrupt_handler(128, syscall_handler);

    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("[SYSCALL] int 0x80 registered (DPL=3)\n");
}
