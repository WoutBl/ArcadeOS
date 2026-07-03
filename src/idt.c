/*
 * ArcadeOS – IDT Setup, PIC Remap, and ISR Dispatcher
 */

#include "idt.h"
#include "vga.h"
#include "syscall.h"

/* ════════════════════════ GDT ════════════════════════ */

/*
 * 6 entries: Null, Kernel Code, Kernel Data, User Code, User Data, TSS
 * Selectors: 0x00, 0x08, 0x10, 0x18, 0x20, 0x28
 */
static gdt_entry_t gdt_entries[6];
static gdt_ptr_t   gdt_pointer;

/* TSS is defined in syscall.c */
extern tss_t kernel_tss;

static void gdt_set_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt_entries[num].base_low    = (uint16_t)(base & 0xFFFF);
    gdt_entries[num].base_middle = (uint8_t)((base >> 16) & 0xFF);
    gdt_entries[num].base_high   = (uint8_t)((base >> 24) & 0xFF);
    gdt_entries[num].limit_low   = (uint16_t)(limit & 0xFFFF);
    gdt_entries[num].granularity  = (uint8_t)(((limit >> 16) & 0x0F) | (gran & 0xF0));
    gdt_entries[num].access      = access;
}

void gdt_init(void) {
    gdt_pointer.limit = sizeof(gdt_entries) - 1;
    gdt_pointer.base  = (uint32_t)&gdt_entries;

    gdt_set_gate(0, 0, 0,          0,    0);    /* Null segment */
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF); /* Kernel code: DPL 0, exec/read */
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF); /* Kernel data: DPL 0, read/write */
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF); /* User code:   DPL 3, exec/read */
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF); /* User data:   DPL 3, read/write */

    /* TSS entry (entry 5, selector 0x28) */
    uint32_t tss_base  = (uint32_t)&kernel_tss;
    uint32_t tss_limit = sizeof(tss_t) - 1;
    gdt_set_gate(5, tss_base, tss_limit, 0x89, 0x00); /* Present, DPL 0, 32-bit TSS */

    gdt_flush(&gdt_pointer);
}

/* ════════════════════════ IDT ════════════════════════ */

/* IDT: 256 entries */
static idt_entry_t idt_entries[256];
static idt_ptr_t   idt_ptr;

/* Registered C-level handlers (one per interrupt number) */
static isr_handler_t interrupt_handlers[256];

/* ──────── Load the IDT register ──────── */
static inline void idt_load(idt_ptr_t* ptr) {
    asm volatile("lidt (%0)" : : "r"(ptr));
}

/* ──────── Set a single IDT gate ──────── */
void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt_entries[num].base_low  = base & 0xFFFF;
    idt_entries[num].base_high = (base >> 16) & 0xFFFF;
    idt_entries[num].selector  = sel;
    idt_entries[num].always0   = 0;
    idt_entries[num].flags     = flags;  /* 0x8E = present, ring 0, 32-bit interrupt gate */
}

/* ──────── Remap the PIC ──────── */
static void pic_remap(void) {
    /* Save masks */
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    /* Start initialization sequence (ICW1) */
    outb(PIC1_COMMAND, 0x11); io_wait();
    outb(PIC2_COMMAND, 0x11); io_wait();

    /* ICW2: Vector offsets */
    outb(PIC1_DATA, 0x20); io_wait();   /* IRQ 0-7  → INT 32-39 */
    outb(PIC2_DATA, 0x28); io_wait();   /* IRQ 8-15 → INT 40-47 */

    /* ICW3: Tell Master PIC that Slave is at IRQ2 (bit 2) */
    outb(PIC1_DATA, 0x04); io_wait();
    /* ICW3: Tell Slave PIC its cascade identity (2) */
    outb(PIC2_DATA, 0x02); io_wait();

    /* ICW4: 8086 mode */
    outb(PIC1_DATA, 0x01); io_wait();
    outb(PIC2_DATA, 0x01); io_wait();

    /* Restore saved masks */
    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
}

/* ──────── Register a C handler for interrupt N ──────── */
void register_interrupt_handler(uint8_t n, isr_handler_t handler) {
    interrupt_handlers[n] = handler;
}

/* ──────── C-level ISR dispatcher (called from isr_common_stub) ──────── */
void isr_handler(registers_t* regs) {
    /*
     * Send End-Of-Interrupt BEFORE calling the handler.
     * This is critical for the scheduler: switch_task() inside
     * the IRQ0 handler changes the stack, so the post-handler
     * EOI code would never execute for the old task. Sending EOI
     * first ensures the PIC allows future IRQs after a context switch.
     */
    if (regs->int_no >= 40) {
        outb(PIC2_COMMAND, PIC_EOI);  /* Slave PIC */
    }
    if (regs->int_no >= 32) {
        outb(PIC1_COMMAND, PIC_EOI);  /* Master PIC */
    }

    /* Dispatch to the registered handler */
    if (interrupt_handlers[regs->int_no] != 0) {
        interrupt_handlers[regs->int_no](regs);
    } else if (regs->int_no < 32) {
        /* Unhandled CPU exception – print error and halt */
        terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_RED));
        terminal_writestring("\n*** [FATAL] KERNEL PANIC: ");
        if (regs->int_no == 8) terminal_writestring("DOUBLE FAULT");
        else {
            terminal_writestring("CPU EXCEPTION ");
            terminal_writedec(regs->int_no);
        }
        terminal_writestring(" ***\nEIP: 0x");
        terminal_writehex(regs->eip);
        terminal_writestring("\nSystem halted.\n");
        cli();
        for (;;) hlt();
    }
}

/* ──────── Initialize the IDT ──────── */
void idt_init(void) {
    idt_ptr.limit = sizeof(idt_entries) - 1;
    idt_ptr.base  = (uint32_t)&idt_entries;

    /* Zero out everything */
    memset(&idt_entries, 0, sizeof(idt_entries));
    memset(&interrupt_handlers, 0, sizeof(interrupt_handlers));

    /* Remap the PIC before setting gates */
    pic_remap();

    /* CPU exceptions (ISR 0–31) */
    idt_set_gate(0,  (uint32_t)isr0,  0x08, 0x8E);
    idt_set_gate(1,  (uint32_t)isr1,  0x08, 0x8E);
    idt_set_gate(2,  (uint32_t)isr2,  0x08, 0x8E);
    idt_set_gate(3,  (uint32_t)isr3,  0x08, 0x8E);
    idt_set_gate(4,  (uint32_t)isr4,  0x08, 0x8E);
    idt_set_gate(5,  (uint32_t)isr5,  0x08, 0x8E);
    idt_set_gate(6,  (uint32_t)isr6,  0x08, 0x8E);
    idt_set_gate(7,  (uint32_t)isr7,  0x08, 0x8E);
    idt_set_gate(8,  (uint32_t)isr8,  0x08, 0x8E);
    idt_set_gate(9,  (uint32_t)isr9,  0x08, 0x8E);
    idt_set_gate(10, (uint32_t)isr10, 0x08, 0x8E);
    idt_set_gate(11, (uint32_t)isr11, 0x08, 0x8E);
    idt_set_gate(12, (uint32_t)isr12, 0x08, 0x8E);
    idt_set_gate(13, (uint32_t)isr13, 0x08, 0x8E);
    idt_set_gate(14, (uint32_t)isr14, 0x08, 0x8E);
    idt_set_gate(15, (uint32_t)isr15, 0x08, 0x8E);
    idt_set_gate(16, (uint32_t)isr16, 0x08, 0x8E);
    idt_set_gate(17, (uint32_t)isr17, 0x08, 0x8E);
    idt_set_gate(18, (uint32_t)isr18, 0x08, 0x8E);
    idt_set_gate(19, (uint32_t)isr19, 0x08, 0x8E);
    idt_set_gate(20, (uint32_t)isr20, 0x08, 0x8E);
    idt_set_gate(21, (uint32_t)isr21, 0x08, 0x8E);
    idt_set_gate(22, (uint32_t)isr22, 0x08, 0x8E);
    idt_set_gate(23, (uint32_t)isr23, 0x08, 0x8E);
    idt_set_gate(24, (uint32_t)isr24, 0x08, 0x8E);
    idt_set_gate(25, (uint32_t)isr25, 0x08, 0x8E);
    idt_set_gate(26, (uint32_t)isr26, 0x08, 0x8E);
    idt_set_gate(27, (uint32_t)isr27, 0x08, 0x8E);
    idt_set_gate(28, (uint32_t)isr28, 0x08, 0x8E);
    idt_set_gate(29, (uint32_t)isr29, 0x08, 0x8E);
    idt_set_gate(30, (uint32_t)isr30, 0x08, 0x8E);
    idt_set_gate(31, (uint32_t)isr31, 0x08, 0x8E);

    /* Hardware IRQs (IRQ 0–15 → ISR 32–47) */
    idt_set_gate(32, (uint32_t)irq0,  0x08, 0x8E);
    idt_set_gate(33, (uint32_t)irq1,  0x08, 0x8E);
    idt_set_gate(34, (uint32_t)irq2,  0x08, 0x8E);
    idt_set_gate(35, (uint32_t)irq3,  0x08, 0x8E);
    idt_set_gate(36, (uint32_t)irq4,  0x08, 0x8E);
    idt_set_gate(37, (uint32_t)irq5,  0x08, 0x8E);
    idt_set_gate(38, (uint32_t)irq6,  0x08, 0x8E);
    idt_set_gate(39, (uint32_t)irq7,  0x08, 0x8E);
    idt_set_gate(40, (uint32_t)irq8,  0x08, 0x8E);
    idt_set_gate(41, (uint32_t)irq9,  0x08, 0x8E);
    idt_set_gate(42, (uint32_t)irq10, 0x08, 0x8E);
    idt_set_gate(43, (uint32_t)irq11, 0x08, 0x8E);
    idt_set_gate(44, (uint32_t)irq12, 0x08, 0x8E);
    idt_set_gate(45, (uint32_t)irq13, 0x08, 0x8E);
    idt_set_gate(46, (uint32_t)irq14, 0x08, 0x8E);
    idt_set_gate(47, (uint32_t)irq15, 0x08, 0x8E);

    idt_load(&idt_ptr);
}
