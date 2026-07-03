; ArcadeOS ISR / IRQ Assembly Stubs
; These provide the low-level interrupt entry points that save CPU state,
; call the C handler, restore state, and iret.

[BITS 32]

; ──────────── C handler (defined in idt.c) ────────────
extern isr_handler

; ──────────── GDT flush routine ───────────────────────
; Called from C: gdt_flush(gdt_ptr_t* ptr)
; Loads the new GDT and reloads all segment registers.
global gdt_flush
gdt_flush:
    mov eax, [esp + 4]  ; Get pointer to gdt_ptr_t
    lgdt [eax]          ; Load the GDT

    ; Reload CS via a far jump
    jmp 0x08:.flush_cs
.flush_cs:
    ; Reload all data segment registers with 0x10 (kernel data)
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    ret

; ──────────── Common stub ─────────────────────────────
isr_common_stub:
    pusha               ; Push edi, esi, ebp, esp, ebx, edx, ecx, eax
    mov ax, ds
    push eax            ; Save data segment

    mov ax, 0x10        ; Load kernel data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp            ; Push pointer to registers_t struct
    call isr_handler
    add esp, 4          ; Clean up pushed argument

    pop eax             ; Restore original data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    popa                ; Restore registers
    add esp, 8          ; Remove int_no and err_code from stack
    iret

; ──────────── ISR macro (no error code) ───────────────
%macro ISR_NOERRCODE 1
global isr%1
isr%1:
    push dword 0        ; Dummy error code
    push dword %1       ; Interrupt number
    jmp isr_common_stub
%endmacro

; ──────────── ISR macro (CPU pushes error code) ───────
%macro ISR_ERRCODE 1
global isr%1
isr%1:
    push dword %1       ; Interrupt number (error code already on stack)
    jmp isr_common_stub
%endmacro

; ──────────── IRQ macro ───────────────────────────────
%macro IRQ 2
global irq%1
irq%1:
    push dword 0        ; Dummy error code
    push dword %2       ; Remapped interrupt number
    jmp isr_common_stub
%endmacro

; ──────────── CPU exception stubs (ISR 0–31) ─────────
ISR_NOERRCODE  0   ; Division By Zero
ISR_NOERRCODE  1   ; Debug
ISR_NOERRCODE  2   ; Non Maskable Interrupt
ISR_NOERRCODE  3   ; Breakpoint
ISR_NOERRCODE  4   ; Overflow
ISR_NOERRCODE  5   ; Bound Range Exceeded
ISR_NOERRCODE  6   ; Invalid Opcode
ISR_NOERRCODE  7   ; Device Not Available
ISR_ERRCODE    8   ; Double Fault
ISR_NOERRCODE  9   ; Coprocessor Segment Overrun
ISR_ERRCODE   10   ; Invalid TSS
ISR_ERRCODE   11   ; Segment Not Present
ISR_ERRCODE   12   ; Stack-Segment Fault
ISR_ERRCODE   13   ; General Protection Fault
ISR_ERRCODE   14   ; Page Fault
ISR_NOERRCODE 15   ; Reserved
ISR_NOERRCODE 16   ; x87 Floating-Point Exception
ISR_ERRCODE   17   ; Alignment Check
ISR_NOERRCODE 18   ; Machine Check
ISR_NOERRCODE 19   ; SIMD Floating-Point Exception
ISR_NOERRCODE 20   ; Virtualization Exception
ISR_NOERRCODE 21   ; Reserved
ISR_NOERRCODE 22   ; Reserved
ISR_NOERRCODE 23   ; Reserved
ISR_NOERRCODE 24   ; Reserved
ISR_NOERRCODE 25   ; Reserved
ISR_NOERRCODE 26   ; Reserved
ISR_NOERRCODE 27   ; Reserved
ISR_NOERRCODE 28   ; Reserved
ISR_NOERRCODE 29   ; Reserved
ISR_ERRCODE   30   ; Security Exception
ISR_NOERRCODE 31   ; Reserved

; ──────────── Hardware IRQ stubs (IRQ 0–15 → ISR 32–47) ──
IRQ  0, 32   ; PIT Timer
IRQ  1, 33   ; Keyboard
IRQ  2, 34   ; Cascade
IRQ  3, 35   ; COM2
IRQ  4, 36   ; COM1
IRQ  5, 37   ; LPT2
IRQ  6, 38   ; Floppy Disk
IRQ  7, 39   ; LPT1 / Spurious
IRQ  8, 40   ; CMOS Real-Time Clock
IRQ  9, 41   ; Free / Legacy SCSI / NIC
IRQ 10, 42   ; Free / SCSI / NIC
IRQ 11, 43   ; Free / SCSI / NIC
IRQ 12, 44   ; PS/2 Mouse
IRQ 13, 45   ; FPU / Coprocessor / Inter-processor
IRQ 14, 46   ; Primary ATA Hard Disk
IRQ 15, 47   ; Secondary ATA Hard Disk

; ──────────── System call stub (int 0x80 = ISR 128) ───
global isr128
isr128:
    push dword 0        ; Dummy error code
    push dword 128      ; Interrupt number (0x80)
    jmp isr_common_stub

; ──────────── TSS flush (load Task Register) ──────────
; Called from C: tss_flush()
global tss_flush
tss_flush:
    mov ax, 0x28        ; TSS selector in the GDT (entry 5, offset 0x28)
    ltr ax              ; Load Task Register
    ret

; ──────────── Enter User Mode (Ring 3 iret trick) ─────
; Called from C: enter_user_mode(uint32_t entry_point, uint32_t user_stack)
; This sets up the stack for iret so the CPU "returns" into Ring 3.
global enter_user_mode
enter_user_mode:
    cli                         ; Disable interrupts during transition

    ; Get arguments from the current (kernel) stack
    mov eax, [esp + 4]          ; arg1: entry_point (EIP for user code)
    mov ecx, [esp + 8]          ; arg2: user_stack  (ESP for user stack)

    ; Set data segment registers to User Data (0x20 | RPL 3 = 0x23)
    mov dx, 0x23
    mov ds, dx
    mov es, dx
    mov fs, dx
    mov gs, dx

    ; Build the iret frame on the KERNEL stack:
    ;   [SS]      = User Data Segment (0x23)
    ;   [ESP]     = User Stack Pointer
    ;   [EFLAGS]  = Current EFLAGS with IF enabled
    ;   [CS]      = User Code Segment (0x18 | RPL 3 = 0x1B)
    ;   [EIP]     = User entry point
    push dword 0x23             ; SS  = user data segment
    push ecx                    ; ESP = user stack pointer
    pushf                       ; EFLAGS
    or dword [esp], 0x200       ; Ensure IF (Interrupt Flag) is set
    push dword 0x1B             ; CS  = user code segment
    push eax                    ; EIP = user entry point

    ; "Return" to Ring 3!
    iret
