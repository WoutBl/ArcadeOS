; ArcadeOS ISR / IRQ Assembly Stubs (x86-64)
; These provide the low-level interrupt entry points that save CPU state,
; call the C handler, restore state, and iretq.
;
; In long mode the CPU always pushes SS:RSP on an interrupt (even for
; ring 0 → ring 0), so the frame layout is uniform and matches the
; registers_t struct in idt.h exactly. The push order below MUST stay in
; sync with that struct (last push = lowest address = first field).

BITS 64

; ──────────── C handler (defined in idt.c) ────────────
extern isr_handler

; ──────────── GDT flush routine ───────────────────────
; Called from C: gdt_flush(gdt_ptr_t* ptr)   (RDI = ptr)
; Loads the new GDT and reloads all segment registers.
global gdt_flush
gdt_flush:
    lgdt [rdi]

    ; Reload data segments with 0x10 (kernel data)
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Reload CS via a far return (no far jmp to a register in long mode)
    push 0x08
    lea rax, [rel .flush_cs]
    push rax
    retfq
.flush_cs:
    ret

; ──────────── Common stub ─────────────────────────────
isr_common_stub:
    ; Push order = registers_t fields from HIGH address down:
    ;   rax, rcx, rdx, rbx, rbp, rsi, rdi, r8..r15, ds
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov ax, ds
    push rax            ; Save data segment

    mov ax, 0x10        ; Load kernel data segment
    mov ds, ax
    mov es, ax

    mov rdi, rsp        ; Arg 1: pointer to registers_t
    call isr_handler

    pop rax             ; Restore original data segment
    mov ds, ax
    mov es, ax

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax

    add rsp, 16         ; Remove int_no and err_code from stack
    iretq

; ──────────── ISR macro (no error code) ───────────────
%macro ISR_NOERRCODE 1
global isr%1
isr%1:
    push qword 0        ; Dummy error code
    push qword %1       ; Interrupt number
    jmp isr_common_stub
%endmacro

; ──────────── ISR macro (CPU pushes error code) ───────
%macro ISR_ERRCODE 1
global isr%1
isr%1:
    push qword %1       ; Interrupt number (error code already on stack)
    jmp isr_common_stub
%endmacro

; ──────────── IRQ macro ───────────────────────────────
%macro IRQ 2
global irq%1
irq%1:
    push qword 0        ; Dummy error code
    push qword %2       ; Remapped interrupt number
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
    push qword 0        ; Dummy error code
    push qword 128      ; Interrupt number (0x80)
    jmp isr_common_stub

; ──────────── TSS flush (load Task Register) ──────────
; Called from C: tss_flush()
global tss_flush
tss_flush:
    mov ax, 0x28        ; TSS selector in the GDT (entries 5+6, offset 0x28)
    ltr ax
    ret

; ──────────── Enter User Mode (Ring 3 iretq trick) ────
; Called from C:
;   enter_user_mode(entry_point, user_stack, argc, argv)
;   RDI = entry, RSI = user stack, RDX = argc, RCX = argv
; Builds an iretq frame so the CPU "returns" into Ring 3, with
; argc/argv placed in RDI/RSI per the SysV calling convention
; (user apps are entered directly at main()).
global enter_user_mode
enter_user_mode:
    cli                         ; Disable interrupts during transition

    ; Set data segment registers to User Data (0x20 | RPL 3 = 0x23)
    mov ax, 0x23
    mov ds, ax
    mov es, ax

    ; Build the iretq frame on the KERNEL stack
    push qword 0x23             ; SS  = user data segment
    push rsi                    ; RSP = user stack pointer
    pushfq                      ; RFLAGS
    or qword [rsp], 0x200       ; Ensure IF (Interrupt Flag) is set
    push qword 0x1B             ; CS  = user code segment (0x18 | RPL 3)
    push rdi                    ; RIP = user entry point

    ; main(argc, argv) arguments for the 64-bit user ABI
    mov rdi, rdx                ; argc
    mov rsi, rcx                ; argv

    ; "Return" to Ring 3!
    iretq
