; ArcadeOS Bootloader
; Multiboot specification for GRUB
;
; Requests a 640x480x32 linear framebuffer from GRUB (FLAGS bit 2) so the
; console boots straight into a graphical mode suitable for games.

MBALIGN  equ  1 << 0                   ; align loaded modules on page boundaries
MEMINFO  equ  1 << 1                   ; provide memory map
VIDINFO  equ  1 << 2                   ; request a video mode (fields below)
FLAGS    equ  MBALIGN | MEMINFO | VIDINFO ; Multiboot 'flag' field
MAGIC    equ  0x1BADB002               ; 'magic number' lets bootloader find the header
CHECKSUM equ -(MAGIC + FLAGS)          ; checksum of above

section .multiboot
align 4
    dd MAGIC
    dd FLAGS
    dd CHECKSUM
    ; Address fields (unused – only valid when FLAGS bit 16 is set)
    dd 0                ; header_addr
    dd 0                ; load_addr
    dd 0                ; load_end_addr
    dd 0                ; bss_end_addr
    dd 0                ; entry_addr
    ; Video mode request (valid because FLAGS bit 2 is set)
    dd 0                ; mode_type: 0 = linear graphics mode
    dd 640              ; width  (pixels)
    dd 480              ; height (pixels)
    dd 32               ; depth  (bits per pixel)

section .bss
align 16
stack_bottom:
resb 32768 ; 32 KiB stack (increased for ISR nesting)
stack_top:

section .text
global _start
global stack_top
_start:
    ; Set up the stack
    mov esp, stack_top

    ; GRUB passes:
    ;   EAX = multiboot magic number (0x2BADB002)
    ;   EBX = pointer to multiboot_info_t struct
    ; Push them as arguments to kernel_main(magic, mboot_info)
    push ebx        ; 2nd argument: multiboot_info_t*
    push eax        ; 1st argument: magic number

    ; Call the kernel main function
    extern kernel_main
    call kernel_main

    ; Clean up stack (not strictly needed, but good practice)
    add esp, 8

    ; Hang if kernel_main returns
    cli
.hang:
    hlt
    jmp .hang
