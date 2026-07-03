#ifndef SYSCALL_H
#define SYSCALL_H

#include "types.h"
#include "idt.h"

/* System call numbers (passed in EAX) */
#define SYS_EXIT   0
#define SYS_WRITE  1
#define SYS_YIELD  2
#define SYS_READ   3
#define SYS_SPAWN  4
#define SYS_WAIT   5
#define SYS_LISTDIR 6
#define SYS_READFILE 7
#define SYS_TOUCH  8
#define SYS_RM     9
#define SYS_MKDIR  10
#define SYS_CD     11
#define SYS_PWD    12
#define SYS_WRITEFILE 13
#define SYS_DATE   14
#define SYS_THEME  15
#define SYS_OPEN   16
#define SYS_CLOSE  17
#define SYS_PIPE   18
#define SYS_DUP2   19
#define SYS_SIGNAL 20

/* Console subsystem syscalls */
#define SYS_GFX_INFO    21   /* EBX = gfx_info_t* out */
#define SYS_GFX_PRESENT 22   /* EBX = uint32_t* pixel buffer (width*height) */
#define SYS_PAD_READ    23   /* EBX = pad index, ECX = pad_state_t* out */
#define SYS_TICKS       24   /* Returns system_ticks (ms since boot) */
#define SYS_MSLEEP      25   /* EBX = milliseconds to sleep */
#define SYS_READDIR     26   /* EBX = path, ECX = index, EDX = dirent_info_t* out */
#define SYS_SAVE        27   /* EBX = filename, ECX = buf, EDX = len (whole-file write) */
#define SYS_LOAD        28   /* EBX = filename, ECX = buf, EDX = maxlen; returns bytes */

/* GDT segment selectors */
#define GDT_KERNEL_CODE  0x08
#define GDT_KERNEL_DATA  0x10
#define GDT_USER_CODE    0x1B   /* 0x18 | RPL 3 */
#define GDT_USER_DATA    0x23   /* 0x20 | RPL 3 */
#define GDT_TSS          0x28

/* TSS structure (x86 hardware requirement: 104 bytes) */
typedef struct {
    uint32_t prev_tss;
    uint32_t esp0;       /* Kernel stack pointer (used on ring 3→0 transition) */
    uint32_t ss0;        /* Kernel stack segment (0x10) */
    uint32_t esp1;
    uint32_t ss1;
    uint32_t esp2;
    uint32_t ss2;
    uint32_t cr3;
    uint32_t eip;
    uint32_t eflags;
    uint32_t eax, ecx, edx, ebx;
    uint32_t esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed)) tss_t;

/* Public API */
void syscall_init(void);
void tss_init(uint32_t kernel_stack_top);
void tss_set_kernel_stack(uint32_t stack_top);

/* Assembly routines (in isr.asm) */
extern void isr128(void);           /* int 0x80 stub */
extern void tss_flush(void);        /* Load TSS selector into TR */
extern void enter_user_mode(uint32_t entry_point, uint32_t user_stack);

#endif /* SYSCALL_H */
