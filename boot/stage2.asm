; ArcadeOS stage-2 bootloader
;
; Loaded by stage 1 at 0000:7E00 with DL = BIOS boot drive. Replaces
; everything GRUB used to do for us:
;
;   1. Enable the A20 line
;   2. Collect the E820 memory map (stored in multiboot mmap format)
;   3. Load the kernel flat binary from the reserved sectors to 1 MiB
;      (chunked: real-mode INT 13h read to a low buffer, then a brief
;      switch to protected mode to copy above 1 MiB)
;   4. Find and set a 640x480x32 linear-framebuffer VBE mode
;   5. Build a multiboot-compatible boot info struct
;   6. Build identity page tables (first 4 GiB, 2 MiB pages), enable
;      long mode, and jump to the 64-bit kernel at 0x100000 with
;      EDI = ARCADEBOOT_MAGIC, ESI = boot info pointer (SysV args)
;
; Low-memory layout:
;   0x00500  memory map (multiboot format: u32 size(=20) + 20-byte E820 entry)
;   0x00800  boot info struct (multiboot_info_t layout, 116 bytes)
;   0x00900  VBE controller info block (512 bytes)
;   0x00B00  VBE mode info block (256 bytes)
;   0x07000  real-mode stack (grows down, set by stage 1)
;   0x07C00  stage 1 (patched sector counts read from offsets 504/508)
;   0x07E00  this code (multiple sectors — nothing else below 0x10000!)
;   0x10000  32 KiB disk chunk buffer
;   0x18000  boot page tables: PML4, PDPT, 4x PD (identity, 2 MiB pages)
;   0x100000 kernel

BITS 16
ORG 0x7E00

%define MMAP_BUF        0x0500
%define BOOTINFO        0x0800
%define VBE_INFO        0x0900
%define VBE_MODEINFO    0x0B00
%define CHUNK_SEG       0x1000          ; chunk buffer = 0x1000:0000 = 0x10000
%define CHUNK_SECTORS   64              ; 32 KiB per chunk
%define KERNEL_DEST     0x100000
%define STAGE1          0x7C00
%define PAGETABLES      0x18000         ; 6 pages: PML4, PDPT, PD0..PD3
%define ARCADE_MAGIC    0xA5CADE05

; multiboot_info_t field offsets (include/multiboot.h)
%define MB_FLAGS        0
%define MB_MEM_LOWER    4
%define MB_MEM_UPPER    8
%define MB_MMAP_LENGTH  44
%define MB_MMAP_ADDR    48
%define MB_FB_ADDR_LO   88
%define MB_FB_ADDR_HI   92
%define MB_FB_PITCH     96
%define MB_FB_WIDTH     100
%define MB_FB_HEIGHT    104
%define MB_FB_BPP       108
%define MB_FB_TYPE      109
%define MB_FB_RED_POS   110
%define MB_FB_RED_SIZE  111
%define MB_FB_GRN_POS   112
%define MB_FB_GRN_SIZE  113
%define MB_FB_BLU_POS   114
%define MB_FB_BLU_SIZE  115

%define MB_FLAG_MEM     (1 << 0)
%define MB_FLAG_MMAP    (1 << 6)
%define MB_FLAG_FB      (1 << 12)

start:
    mov [boot_drive], dl
    cld

    mov si, msg_banner
    call print

    ; ---- Zero the boot info struct ----
    xor ax, ax
    mov es, ax
    mov di, BOOTINFO
    mov cx, 116 / 2
    rep stosw

    ; ---- 1. Enable A20 ----
    mov ax, 0x2401              ; BIOS A20 enable (may be unsupported; harmless)
    int 0x15
    in al, 0x92                 ; fast A20 gate
    or al, 0x02
    and al, 0xFE                ; never touch the CPU-reset bit
    out 0x92, al

    ; ---- 2. Memory detection ----
    int 0x12                    ; AX = KiB of conventional memory
    mov [BOOTINFO + MB_MEM_LOWER], ax
    mov word [BOOTINFO + MB_MEM_LOWER + 2], 0

    ; E820 memory map, stored as multiboot entries (u32 size + 20 bytes)
    xor ebx, ebx                ; continuation value
    mov di, MMAP_BUF + 4        ; BIOS writes the 20-byte payload here
    xor bp, bp                  ; entry count
.e820_loop:
    mov eax, 0xE820
    mov edx, 0x534D4150         ; 'SMAP'
    mov ecx, 24
    int 0x15
    jc .e820_done
    cmp eax, 0x534D4150
    jne .e820_done
    mov dword [di - 4], 20      ; multiboot size prefix
    inc bp
    add di, 24
    cmp bp, 32                  ; buffer cap
    jae .e820_done
    test ebx, ebx
    jnz .e820_loop
.e820_done:
    test bp, bp
    jnz .have_mmap

    ; E820 unsupported: fall back to INT 15h AH=88h and fake a single entry
    mov ah, 0x88
    int 0x15                    ; AX = KiB above 1 MiB (caps at 64 MiB ranges)
    movzx eax, ax
    mov [BOOTINFO + MB_MEM_UPPER], eax
    mov dword [MMAP_BUF +  0], 20
    mov dword [MMAP_BUF +  4], 0x100000   ; base low
    mov dword [MMAP_BUF +  8], 0          ; base high
    shl eax, 10                           ; KiB -> bytes
    mov [MMAP_BUF + 12], eax              ; length low
    mov dword [MMAP_BUF + 16], 0          ; length high
    mov dword [MMAP_BUF + 20], 1          ; type: available
    mov bp, 1
    jmp .mmap_stored

.have_mmap:
    ; mem_upper = largest "available" span covering 1 MiB
    xor edx, edx                ; best candidate (KiB)
    mov si, MMAP_BUF
    mov cx, bp
.mem_upper_loop:
    cmp dword [si + 20], 1      ; type == available?
    jne .mem_upper_next
    cmp dword [si + 8], 0       ; base above 4 GiB: skip (32-bit kernel)
    jne .mem_upper_next
    mov eax, [si + 4]           ; base low
    cmp eax, 0x100000
    ja  .mem_upper_next         ; region starts above 1 MiB
    add eax, [si + 12]          ; end = base + length (low dword)
    jnc .no_sat
    mov eax, 0xFFFFFFFF         ; saturate on 4 GiB overflow
.no_sat:
    cmp eax, 0x100000
    jbe .mem_upper_next
    sub eax, 0x100000
    shr eax, 10                 ; bytes -> KiB
    cmp eax, edx
    jbe .mem_upper_next
    mov edx, eax
.mem_upper_next:
    add si, 24
    loop .mem_upper_loop
    mov [BOOTINFO + MB_MEM_UPPER], edx

.mmap_stored:
    mov ax, bp
    mov dx, 24
    mul dx                      ; AX = count * 24
    movzx eax, ax
    mov [BOOTINFO + MB_MMAP_LENGTH], eax
    mov dword [BOOTINFO + MB_MMAP_ADDR], MMAP_BUF

    ; ---- 3. Load the kernel to 1 MiB ----
    mov si, msg_kernel
    call print

    mov eax, [STAGE1 + 504]     ; kernel size in sectors (patched by mkfat32)
    mov [sectors_left], eax
    movzx eax, word [STAGE1 + 508]
    add eax, 8                  ; kernel LBA = 8 + stage2 sectors
    mov [cur_lba], eax
    mov dword [copy_dest], KERNEL_DEST

.load_loop:
    mov eax, [sectors_left]
    test eax, eax
    jz .load_done
    cmp eax, CHUNK_SECTORS
    jbe .chunk_sized
    mov eax, CHUNK_SECTORS
.chunk_sized:
    mov [chunk_sectors], ax

    ; INT 13h extended read: chunk -> 0x1000:0000
    mov [dap_count], ax
    mov eax, [cur_lba]
    mov [dap_lba], eax
    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jc .disk_fail

    ; Copy the chunk above 1 MiB via a protected-mode round trip
    movzx ecx, word [chunk_sectors]
    shl ecx, 7                  ; sectors * 512 / 4 = dwords
    mov [copy_dwords], ecx
    call pm_copy

    ; Advance
    movzx eax, word [chunk_sectors]
    add [cur_lba], eax
    sub [sectors_left], eax
    shl eax, 9
    add [copy_dest], eax

    mov si, msg_dot
    call print
    jmp .load_loop

.disk_fail:
    mov si, msg_disk_err
    call print
    jmp halt

.load_done:
    mov si, msg_crlf
    call print

    ; ---- 4. VBE: find and set 640x480x32 with a linear framebuffer ----
    mov di, VBE_INFO
    mov dword [di], "VBE2"      ; request VBE 2.0+ info
    mov ax, 0x4F00
    int 0x10
    cmp ax, 0x004F
    jne .vbe_fail

    ; Walk the mode list (far pointer at VbeInfoBlock+0x0E)
    mov si, [VBE_INFO + 0x0E]
    mov ax, [VBE_INFO + 0x10]
    mov fs, ax
.mode_loop:
    mov cx, [fs:si]
    cmp cx, 0xFFFF
    je .vbe_fail
    add si, 2

    push si
    mov ax, 0x4F01              ; get mode info
    mov di, VBE_MODEINFO
    int 0x10
    pop si
    cmp ax, 0x004F
    jne .mode_loop

    mov ax, [VBE_MODEINFO + 0x00]   ; ModeAttributes
    and ax, 0x0081                  ; supported + linear framebuffer
    cmp ax, 0x0081
    jne .mode_loop
    cmp byte [VBE_MODEINFO + 0x1B], 6   ; direct color
    jne .mode_loop
    cmp word [VBE_MODEINFO + 0x12], 640
    jne .mode_loop
    cmp word [VBE_MODEINFO + 0x14], 480
    jne .mode_loop
    cmp byte [VBE_MODEINFO + 0x19], 32
    jne .mode_loop

    ; Found it: set the mode with the LFB bit
    mov bx, cx
    or bx, 0x4000
    mov ax, 0x4F02
    int 0x10
    cmp ax, 0x004F
    jne .vbe_fail

    ; ---- 5. Fill the framebuffer fields of the boot info ----
    mov eax, [VBE_MODEINFO + 0x28]  ; PhysBasePtr
    mov [BOOTINFO + MB_FB_ADDR_LO], eax
    mov dword [BOOTINFO + MB_FB_ADDR_HI], 0
    movzx eax, word [VBE_MODEINFO + 0x10]
    mov [BOOTINFO + MB_FB_PITCH], eax
    movzx eax, word [VBE_MODEINFO + 0x12]
    mov [BOOTINFO + MB_FB_WIDTH], eax
    movzx eax, word [VBE_MODEINFO + 0x14]
    mov [BOOTINFO + MB_FB_HEIGHT], eax
    mov al, [VBE_MODEINFO + 0x19]
    mov [BOOTINFO + MB_FB_BPP], al
    mov byte [BOOTINFO + MB_FB_TYPE], 1     ; RGB
    mov al, [VBE_MODEINFO + 0x20]
    mov [BOOTINFO + MB_FB_RED_POS], al
    mov al, [VBE_MODEINFO + 0x1F]
    mov [BOOTINFO + MB_FB_RED_SIZE], al
    mov al, [VBE_MODEINFO + 0x22]
    mov [BOOTINFO + MB_FB_GRN_POS], al
    mov al, [VBE_MODEINFO + 0x21]
    mov [BOOTINFO + MB_FB_GRN_SIZE], al
    mov al, [VBE_MODEINFO + 0x24]
    mov [BOOTINFO + MB_FB_BLU_POS], al
    mov al, [VBE_MODEINFO + 0x23]
    mov [BOOTINFO + MB_FB_BLU_SIZE], al

    mov dword [BOOTINFO + MB_FLAGS], MB_FLAG_MEM | MB_FLAG_MMAP | MB_FLAG_FB
    jmp enter_kernel

.vbe_fail:
    ; No 640x480x32 LFB mode: boot anyway without the FB flag so the
    ; kernel falls back to VGA text mode
    mov si, msg_vbe_err
    call print
    mov dword [BOOTINFO + MB_FLAGS], MB_FLAG_MEM | MB_FLAG_MMAP

; ---- 6. Enter protected mode, then long mode, and jump to the kernel ----
enter_kernel:
    cli
    lgdt [gdt_desc]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp 0x08:pm_entry

BITS 32
pm_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x9F000            ; scratch stack; kernel _start replaces it

    ; Boot page tables at 0x18000 (above the chunk buffer, clear of this
    ; code): identity-map the first 4 GiB with 2 MiB pages (covers RAM +
    ; the framebuffer MMIO). paging_init() replaces these later.
    mov edi, PAGETABLES
    xor eax, eax
    mov ecx, (6 * 4096) / 4     ; PML4 + PDPT + 4 PDs
    rep stosd

    mov dword [PAGETABLES + 0x0000], PAGETABLES + 0x1000 + 0x03  ; PML4[0] -> PDPT
    mov dword [PAGETABLES + 0x1000], PAGETABLES + 0x2000 + 0x03  ; PDPT[0] -> PD0
    mov dword [PAGETABLES + 0x1008], PAGETABLES + 0x3000 + 0x03  ; PDPT[1] -> PD1
    mov dword [PAGETABLES + 0x1010], PAGETABLES + 0x4000 + 0x03  ; PDPT[2] -> PD2
    mov dword [PAGETABLES + 0x1018], PAGETABLES + 0x5000 + 0x03  ; PDPT[3] -> PD3

    mov edi, PAGETABLES + 0x2000
    mov eax, 0x83               ; present | writable | 2 MiB page
    mov ecx, 4 * 512            ; 2048 entries x 2 MiB = 4 GiB
.fill_pd:
    mov [edi], eax
    mov dword [edi + 4], 0
    add eax, 0x200000
    add edi, 8
    loop .fill_pd

    ; PAE -> CR3 -> EFER.LME -> paging on = long mode
    mov eax, cr4
    or eax, 1 << 5              ; CR4.PAE
    mov cr4, eax
    mov eax, PAGETABLES
    mov cr3, eax
    mov ecx, 0xC0000080         ; IA32_EFER
    rdmsr
    or eax, 1 << 8              ; LME
    wrmsr
    mov eax, cr0
    or eax, 0x80000000          ; CR0.PG
    mov cr0, eax
    jmp 0x18:long_entry

BITS 64
long_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov rsp, 0x9F000

    mov edi, ARCADE_MAGIC       ; kernel_main arg 1 (SysV: RDI)
    mov esi, BOOTINFO           ; kernel_main arg 2 (SysV: RSI)
    mov rax, KERNEL_DEST
    jmp rax

BITS 16

; ---- Copy the chunk buffer to [copy_dest] via protected mode ----
; Interrupts stay off for the whole round trip; segments are restored
; before returning to real mode callers.
pm_copy:
    cli
    lgdt [gdt_desc]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp short .pm               ; flush prefetch
.pm:
    mov bx, 0x10                ; flat 4 GiB data selector
    mov ds, bx
    mov es, bx
    mov esi, CHUNK_SEG << 4
    mov edi, [copy_dest]
    mov ecx, [copy_dwords]
    a32 rep movsd
    mov eax, cr0
    and al, 0xFE
    mov cr0, eax
    jmp short .rm
.rm:
    xor bx, bx
    mov ds, bx
    mov es, bx
    sti
    ret

; ---- BIOS teletype print (SI = zero-terminated string) ----
print:
    lodsb
    or al, al
    jz .done
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    jmp print
.done:
    ret

halt:
    hlt
    jmp halt

; ---- GDT: null, code32 (0x08), data (0x10), code64 (0x18) ----
align 8
gdt:
    dq 0
    dq 0x00CF9A000000FFFF       ; code: base 0, limit 4 GiB, 32-bit
    dq 0x00CF92000000FFFF       ; data: base 0, limit 4 GiB
    dq 0x00AF9A000000FFFF       ; code: 64-bit (L=1)
gdt_desc:
    dw gdt_desc - gdt - 1
    dd gdt

; ---- Disk Address Packet ----
align 4
dap:
    db 0x10, 0x00
dap_count:
    dw 0
    dw 0x0000                   ; destination offset
    dw CHUNK_SEG                ; destination segment
dap_lba:
    dq 0

; ---- Variables ----
boot_drive:     db 0
chunk_sectors:  dw 0
sectors_left:   dd 0
cur_lba:        dd 0
copy_dest:      dd 0
copy_dwords:    dd 0

msg_banner:   db "ArcadeOS boot", 13, 10, 0
msg_kernel:   db "Loading kernel", 0
msg_dot:      db ".", 0
msg_crlf:     db 13, 10, 0
msg_disk_err: db 13, 10, "kernel read failed", 13, 10, 0
msg_vbe_err:  db "no 640x480x32 VBE mode", 13, 10, 0
