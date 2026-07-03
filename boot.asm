; ArcadeOS kernel entry
;
; The ArcadeOS bootloader (boot/stage1.asm + boot/stage2.asm) loads the
; kernel as a FLAT BINARY at 0x100000 and jumps to its first byte, so
; _start must be the first thing in the image (section .text.start, placed
; first by linker.ld). On entry:
;   EAX = ARCADEBOOT_MAGIC (0xA5CADE05)
;   EBX = boot info pointer (multiboot_info_t-compatible layout)
;
; Because a flat binary has no ELF loader to zero NOBITS sections, .bss
; (including this stack) is zeroed here before calling kernel_main.

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
    mov esp, stack_top

    ; Zero .bss: the boot magic lives in EAX which rep stosd clobbers,
    ; so park it in ESI; EBX (boot info) is untouched
    mov esi, eax
    extern __bss_start
    extern kernel_end
    mov edi, __bss_start
    mov ecx, kernel_end
    sub ecx, edi
    shr ecx, 2
    xor eax, eax
    cld
    rep stosd

    ; kernel_main(magic, boot_info)
    push ebx
    push esi
    extern kernel_main
    call kernel_main

    ; Hang if kernel_main returns
    cli
.hang:
    hlt
    jmp .hang
