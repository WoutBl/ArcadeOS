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
#include "fs.h"
#include "clock.h"
#include "vga.h"
#include "vfs.h"
#include "pipe.h"
#include "fb.h"
#include "console_gfx.h"
#include "gamepad.h"
#include "usb.h"
#include "console_abi.h"
#include "fat32.h"
#include "audio.h"

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
            if (len == 0) len = (uint32_t)strlen(str); /* backward compat */

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

            if (!buf || count == 0) { regs->eax = (uint32_t)-1; break; }

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
            if (!path) {
                regs->eax = (uint32_t)-1;
                break;
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

        case SYS_LISTDIR: {
            char* buf = (char*)regs->ebx;
            int max_len = regs->ecx;
            if (!buf || max_len <= 0) {
                regs->eax = (uint32_t)-1;
                break;
            }
            
            buf[0] = '\0';
            int written = 0;
            size_t current_len = strlen(current_dir);
            for (int i = 0; i < MAX_FILES; i++) {
                if (filesystem[i].in_use) {
                    int in_current = 1;
                    for (size_t j = 0; j < current_len; j++) {
                        if (filesystem[i].path[j] != current_dir[j]) { in_current = 0; break; }
                    }
                    if (in_current) {
                        const char* remainder = filesystem[i].path + current_len;
                        if (*remainder == '/') remainder++;

                        int is_direct_child = 1;
                        for (size_t j = 0; remainder[j]; j++) {
                            if (remainder[j] == '/' && remainder[j+1] != '\0') { is_direct_child = 0; break; }
                        }
                        if (strcmp(filesystem[i].path, current_dir) == 0) continue;

                        if (is_direct_child && strlen(remainder) > 0) {
                            const char* prefix = filesystem[i].is_directory ? "[DIR]  " : "[FILE] ";
                            size_t plen = strlen(prefix);
                            size_t nlen = strlen(filesystem[i].name);
                            if (written + plen + nlen + 2 <= (size_t)max_len) {
                                strcpy(buf + written, prefix); written += plen;
                                strcpy(buf + written, filesystem[i].name); written += nlen;
                                buf[written++] = '\n';
                                buf[written] = '\0';
                            }
                        }
                    }
                }
            }
            regs->eax = written;
            break;
        }

        case SYS_READFILE: {
            const char* path = (const char*)regs->ebx;
            char* buf = (char*)regs->ecx;
            int max_len = regs->edx;
            if (!path || !buf || max_len <= 0) {
                regs->eax = (uint32_t)-1;
                break;
            }
            file_t* f = fs_get_file(path);
            if (!f) {
                regs->eax = (uint32_t)-1;
                break;
            }
            int fsize = f->size;
            int to_copy = (fsize < max_len - 1) ? fsize : max_len - 1;
            for (int i=0; i < to_copy; i++) buf[i] = f->content[i];
            buf[to_copy] = '\0';
            regs->eax = to_copy;
            break;
        }

        case SYS_TOUCH: {
            const char* path = (const char*)regs->ebx;
            if (!path) { regs->eax = (uint32_t)-1; break; }
            regs->eax = fs_create_file(path, "");
            break;
        }

        case SYS_RM: {
            const char* path = (const char*)regs->ebx;
            if (!path) { regs->eax = (uint32_t)-1; break; }
            regs->eax = fs_delete_file(path);
            break;
        }

        case SYS_MKDIR: {
            const char* path = (const char*)regs->ebx;
            if (!path) { regs->eax = (uint32_t)-1; break; }
            regs->eax = fs_create_directory(path);
            break;
        }

        case SYS_CD: {
            const char* path = (const char*)regs->ebx;
            if (!path) { regs->eax = (uint32_t)-1; break; }
            regs->eax = fs_change_directory(path);
            break;
        }

        case SYS_PWD: {
            char* buf = (char*)regs->ebx;
            int max_len = regs->ecx;
            if (!buf || max_len <= 0) { regs->eax = (uint32_t)-1; break; }
            int i = 0;
            while (current_dir[i] && i < max_len - 1) { buf[i] = current_dir[i]; i++; }
            buf[i] = '\0';
            regs->eax = 0;
            break;
        }

        case SYS_WRITEFILE: {
            const char* path = (const char*)regs->ebx;
            const char* content = (const char*)regs->ecx;
            if (!path || !content) { regs->eax = (uint32_t)-1; break; }
            regs->eax = fs_create_file(path, content);
            break;
        }

        case SYS_DATE: {
            char* buf = (char*)regs->ebx;
            int max_len = regs->ecx;
            if (!buf || max_len < 20) { regs->eax = (uint32_t)-1; break; }
            uint32_t yy = current_year;
            uint32_t mo = current_month;
            uint32_t dd = current_day;
            uint32_t hh = current_hours;
            uint32_t mm = current_minutes;
            
            char s_yy[5], s_mo[3], s_dd[3], s_hh[3], s_mm[3];
            num_to_str(s_yy, yy, 4);
            num_to_str(s_mo, mo, 2);
            num_to_str(s_dd, dd, 2);
            num_to_str(s_hh, hh, 2);
            num_to_str(s_mm, mm, 2);

            int written = 0;
            const char* comp[] = {s_yy, "-", s_mo, "-", s_dd, " ", s_hh, ":", s_mm, "\0"};
            for (int k = 0; comp[k][0] != '\0'; k++) {
                for (int l = 0; comp[k][l] != '\0'; l++) {
                    if (written < max_len - 1) {
                        buf[written++] = comp[k][l];
                    }
                }
            }
            buf[written] = '\0';
            regs->eax = 0;
            break;
        }

        case SYS_THEME: {
            int theme_id = regs->ebx;
            if (theme_id >= 0 && theme_id < NUM_THEMES) {
                current_theme = theme_id;
                terminal_setcolor(vga_entry_color(themes[current_theme].text_fg, themes[current_theme].text_bg));
                regs->eax = 0;
            } else {
                regs->eax = (uint32_t)-1;
            }
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

            if (!path || !current_task) { regs->eax = (uint32_t)-1; break; }

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

        case SYS_PIPE: {
            /*
             * EBX = pointer to int[2] in user space.
             * Creates a new pipe and allocates two FD slots in the calling
             * process:  pipefd[0] = read end,  pipefd[1] = write end.
             */
            int* pipefd = (int*)regs->ebx;
            if (!pipefd || !current_task) { regs->eax = (uint32_t)-1; break; }

            pipe_buf_t* pb = pipe_create();
            if (!pb) { regs->eax = (uint32_t)-1; break; }

            vfs_node_t* rnode = pipe_make_read_node(pb);
            vfs_node_t* wnode = pipe_make_write_node(pb);
            if (!rnode || !wnode) { regs->eax = (uint32_t)-1; break; }

            /* Find two free FD slots */
            int rfd = -1, wfd = -1;
            for (int i = 3; i < MAX_FD && (rfd < 0 || wfd < 0); i++) {
                if (current_task->fds[i] == (vfs_node_t*)0) {
                    if (rfd < 0)      { current_task->fds[i] = rnode; rfd = i; }
                    else if (wfd < 0) { current_task->fds[i] = wnode; wfd = i; }
                }
            }
            if (rfd < 0 || wfd < 0) { regs->eax = (uint32_t)-1; break; }

            pipefd[0] = rfd;
            pipefd[1] = wfd;
            regs->eax = 0;
            break;
        }

        case SYS_DUP2: {
            /*
             * EBX = oldfd, ECX = newfd.
             * Copies fds[oldfd] → fds[newfd] WITHOUT calling close() on the
             * old node (the underlying pipe stays open via ref count).
             * If newfd already has a node it is silently replaced.
             */
            int oldfd = (int)regs->ebx;
            int newfd = (int)regs->ecx;

            if (!current_task
                || oldfd < 0 || oldfd >= MAX_FD
                || newfd < 0 || newfd >= MAX_FD) {
                regs->eax = (uint32_t)-1;
                break;
            }

            /* Close existing newfd properly ONLY if it differs from oldfd */
            if (oldfd != newfd && current_task->fds[newfd]) {
                vfs_close(current_task->fds[newfd]);
                current_task->fds[newfd] = (vfs_node_t*)0;
            }

            current_task->fds[newfd] = current_task->fds[oldfd];
            regs->eax = (uint32_t)newfd;
            break;
        }

        case SYS_SIGNAL: {
            int signum = (int)regs->ebx;
            void* handler = (void*)regs->ecx;
            
            if (current_task) {
                if ((uintptr_t)handler == 1) { /* SIG_IGN */
                    current_task->ignored_signals |= (1 << signum);
                } else { /* SIG_DFL */
                    current_task->ignored_signals &= ~(1 << signum);
                }
            }
            regs->eax = 0;
            break;
        }

        case SYS_GFX_INFO: {
            /* EBX = gfx_info_t* – report the framebuffer geometry so the
             * game can allocate a matching pixel buffer. */
            gfx_info_t* info = (gfx_info_t*)regs->ebx;
            if (!info || !fb_available()) { regs->eax = (uint32_t)-1; break; }

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
            uint32_t ptr = regs->ebx;
            if (!fb_available()) { regs->eax = (uint32_t)-1; break; }

            uint32_t bytes = fb_width() * fb_height() * 4;
            /* The buffer must live entirely inside user address space */
            if (ptr < 0x400000 || ptr + bytes > 0xC0000000) {
                regs->eax = (uint32_t)-1;
                break;
            }
            gfx_present_buffer((const uint32_t*)(uintptr_t)ptr);
            regs->eax = 0;
            break;
        }

        case SYS_PAD_READ: {
            /* EBX = pad index, ECX = pad_state_t* out */
            int          index = (int)regs->ebx;
            pad_state_t* out   = (pad_state_t*)regs->ecx;
            if (!out) { regs->eax = (uint32_t)-1; break; }

            usb_poll();                    /* Hot-plug + future HID reports */
            gamepad_get_state(index, out);
            regs->eax = 0;
            break;
        }

        case SYS_TICKS:
            /* Milliseconds since boot – for frame timing */
            regs->eax = system_ticks;
            break;

        case SYS_MSLEEP: {
            /* EBX = ms. Sleep with interrupts enabled so the PIT keeps
             * ticking and other tasks get scheduled. */
            /* Pure tick-wait: hlt wakes on the next PIT interrupt (1 ms).
             * Other tasks still run – scheduler_tick preempts us. */
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
            if (!path || !out) { regs->eax = (uint32_t)-1; break; }

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

            if (!name || len > 64 * 1024) { regs->eax = (uint32_t)-1; break; }
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

            if (!name || !buf) { regs->eax = (uint32_t)-1; break; }

            regs->eax = (uint32_t)fat32_load(name, buf, maxlen);
            break;
        }

        case SYS_SOUND: {
            /* EBX = frequency in Hz (0 stops playback), ECX = duration ms */
            audio_tone((uint32_t)regs->ebx, (uint32_t)regs->ecx);
            regs->eax = 0;
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
