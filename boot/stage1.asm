; ArcadeOS stage-1 bootloader (FAT32 Volume Boot Record)
;
; Lives in sector 0 of the game volume. tools/mkfat32.py overwrites
; offsets 0x00-0x59 with the FAT32 BPB (the jump at offset 0 lands on
; code_start at 0x5A) and patches the sector counts at offsets 504/508.
;
; Job: load stage 2 from the reserved sectors (LBA 8) to 0x7E00 and jump.
; The BIOS boot drive stays in DL for stage 2.

BITS 16
ORG 0x7C00

    jmp short code_start            ; EB 58 90 once mkfat32.py patches the BPB
    nop
    times 0x5A - ($ - $$) db 0      ; BPB / EBPB area, filled by mkfat32.py

code_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7000                  ; real-mode stack below the boot sector
    sti
    cld

    mov [boot_drive], dl

    ; Read stage 2 (LBA 8, count patched at offset 508) to 0000:7E00
    mov ax, [stage2_sectors]
    mov [dap_count], ax
    mov si, dap
    mov ah, 0x42                    ; INT 13h extended read (LBA)
    mov dl, [boot_drive]
    int 0x13
    jc disk_error

    mov dl, [boot_drive]
    jmp 0x0000:0x7E00

disk_error:
    mov si, msg_err
.print:
    lodsb
    or al, al
    jz .halt
    mov ah, 0x0E                    ; BIOS teletype
    mov bx, 0x0007
    int 0x10
    jmp .print
.halt:
    hlt
    jmp .halt

; Disk Address Packet for INT 13h AH=42h
align 4
dap:
    db 0x10, 0x00
dap_count:
    dw 0                            ; sectors to read (patched in at runtime)
    dw 0x7E00                       ; destination offset
    dw 0x0000                       ; destination segment
    dq 8                            ; start LBA (stage 2 begins at sector 8)

boot_drive: db 0
msg_err:    db "ArcadeOS: stage2 read failed", 13, 10, 0

; ---- Fixed patch area (written by tools/mkfat32.py) ----
times 504 - ($ - $$) db 0
kernel_sectors: dd 0                ; offset 504: kernel size in sectors
stage2_sectors: dw 0                ; offset 508: stage 2 size in sectors
dw 0xAA55                           ; offset 510: boot signature
