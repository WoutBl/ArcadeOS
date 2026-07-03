#ifndef IDT_H
#define IDT_H

#include "types.h"

/* IDT entry (16 bytes each in long mode) */
typedef struct {
    uint16_t base_low;    /* Handler address bits 0-15 */
    uint16_t selector;    /* Kernel code segment selector */
    uint8_t  ist;         /* Interrupt Stack Table index (0 = none) */
    uint8_t  flags;       /* Type and attributes */
    uint16_t base_mid;    /* Handler address bits 16-31 */
    uint32_t base_high;   /* Handler address bits 32-63 */
    uint32_t reserved;    /* Must be zero */
} __attribute__((packed)) idt_entry_t;

/* Pointer structure for lidt */
typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) idt_ptr_t;

/*
 * Pushed by the ISR stub before calling the C handler.
 *
 * 64-bit port note: the fields keep their historical 32-bit names
 * (eax, eip, ...) but hold the full 64-bit registers (rax, rip, ...) —
 * this keeps the syscall dispatcher and fault handlers source-compatible.
 * The layout MUST match the push order in isr.asm's isr_common_stub.
 */
typedef struct {
    uint64_t ds;                                     /* Saved data segment */
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;   /* Extended GPRs */
    uint64_t edi, esi, ebp, ebx, edx, ecx, eax;      /* rdi..rax */
    uint64_t int_no, err_code;                       /* Interrupt number + error code */
    uint64_t eip, cs, eflags, useresp, ss;           /* iretq frame (always pushed) */
} __attribute__((packed)) registers_t;

/* Handler callback type */
typedef void (*isr_handler_t)(registers_t* regs);

/* GDT entry (8 bytes; the 64-bit TSS descriptor spans two entries) */
typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed)) gdt_entry_t;

/* GDT pointer for lgdt */
typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdt_ptr_t;

/* Assembly routine to load GDT and reload segment registers (in isr.asm) */
extern void gdt_flush(gdt_ptr_t* ptr);

/* Public API */
void gdt_init(void);
void idt_init(void);
void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags);
void register_interrupt_handler(uint8_t n, isr_handler_t handler);

/* PIC ports */
#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1
#define PIC_EOI      0x20

/* IRQ numbers (remapped) */
#define IRQ0  32
#define IRQ1  33
#define IRQ2  34
#define IRQ3  35
#define IRQ4  36
#define IRQ5  37
#define IRQ6  38
#define IRQ7  39
#define IRQ8  40
#define IRQ9  41
#define IRQ10 42
#define IRQ11 43
#define IRQ12 44
#define IRQ13 45
#define IRQ14 46
#define IRQ15 47

/* Assembly ISR stubs (defined in isr.asm) */
extern void isr0(void);
extern void isr1(void);
extern void isr2(void);
extern void isr3(void);
extern void isr4(void);
extern void isr5(void);
extern void isr6(void);
extern void isr7(void);
extern void isr8(void);
extern void isr9(void);
extern void isr10(void);
extern void isr11(void);
extern void isr12(void);
extern void isr13(void);
extern void isr14(void);
extern void isr15(void);
extern void isr16(void);
extern void isr17(void);
extern void isr18(void);
extern void isr19(void);
extern void isr20(void);
extern void isr21(void);
extern void isr22(void);
extern void isr23(void);
extern void isr24(void);
extern void isr25(void);
extern void isr26(void);
extern void isr27(void);
extern void isr28(void);
extern void isr29(void);
extern void isr30(void);
extern void isr31(void);

extern void irq0(void);
extern void irq1(void);
extern void irq2(void);
extern void irq3(void);
extern void irq4(void);
extern void irq5(void);
extern void irq6(void);
extern void irq7(void);
extern void irq8(void);
extern void irq9(void);
extern void irq10(void);
extern void irq11(void);
extern void irq12(void);
extern void irq13(void);
extern void irq14(void);
extern void irq15(void);

#endif /* IDT_H */
