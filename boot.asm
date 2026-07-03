; ArcadeOS kernel entry (x86-64)
;
; The ArcadeOS bootloader (boot/stage1.asm + boot/stage2.asm) loads the
; kernel as a FLAT BINARY at 0x100000, enters long mode, and jumps to the
; first byte, so _start must be the first thing in the image (section
; .text.start, placed first by linker.ld). On entry (SysV convention):
;   RDI = ARCADEBOOT_MAGIC (0xA5CADE05)
;   RSI = boot info pointer (multiboot_info_t-compatible layout)
;
; Because a flat binary has no ELF loader to zero NOBITS sections, .bss
; (including this stack) is zeroed here before calling kernel_main.

BITS 64

section .bss
align 16
stack_bottom:
resb 32768 ; 32 KiB stack (increased for ISR nesting)
stack_top:

section .text.start
global _start
global stack_top
_start:
    ; Set up the stack (in .bss — zeroed below, nothing pushed yet)
    mov rsp, stack_top

    ; Zero .bss: rep stosq clobbers RAX/RCX/RDI, so park the args
    mov r12, rdi
    mov r13, rsi
    extern __bss_start
    extern kernel_end
    mov rdi, __bss_start
    mov rcx, kernel_end
    sub rcx, rdi
    shr rcx, 3
    xor rax, rax
    cld
    rep stosq

    ; kernel_main(magic, boot_info)
    mov rdi, r12
    mov rsi, r13
    extern kernel_main
    call kernel_main

    ; Hang if kernel_main returns
    cli
.hang:
    hlt
    jmp .hang
