/*
 * ArcadeOS – PIT Timer (IRQ0 at 1000 Hz) & CMOS RTC Reader
 */

#include "clock.h"
#include "idt.h"
#include "vga.h"
#include "scheduler.h"

/* ──────── Time state ──────── */
volatile uint32_t system_ticks = 0;
uint32_t boot_time      = 0;
uint32_t current_seconds = 0;
uint32_t current_minutes = 0;
uint32_t current_hours   = 0;
uint32_t current_day     = 1;
uint32_t current_month   = 1;
uint32_t current_year    = 2025;

/* ──────── IRQ0 handler (PIT tick) ──────── */
static void pit_irq_handler(registers_t* regs) {
    (void)regs;
    system_ticks++;

    /* Call the scheduler tick (preemptive multitasking) */
    scheduler_tick();
}

/* ──────── CMOS / RTC helpers ──────── */
static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDRESS, reg);
    return inb(CMOS_DATA);
}

static int cmos_is_updating(void) {
    outb(CMOS_ADDRESS, 0x0A);
    return inb(CMOS_DATA) & 0x80;
}

static uint8_t bcd_to_bin(uint8_t bcd) {
    return (uint8_t)(((bcd >> 4) & 0x0F) * 10 + (bcd & 0x0F));
}

static void rtc_read(void) {
    /* Wait until CMOS is not updating */
    while (cmos_is_updating())
        ;

    uint8_t seconds = cmos_read(0x00);
    uint8_t minutes = cmos_read(0x02);
    uint8_t hours   = cmos_read(0x04);
    uint8_t day     = cmos_read(0x07);
    uint8_t month   = cmos_read(0x08);
    uint8_t year    = cmos_read(0x09);
    uint8_t century = cmos_read(0x32);   /* May be 0 on some hardware */

    /* Read register B to determine format */
    uint8_t regB = cmos_read(0x0B);

    /* Convert BCD to binary if needed */
    if (!(regB & 0x04)) {
        seconds = bcd_to_bin(seconds);
        minutes = bcd_to_bin(minutes);
        hours   = bcd_to_bin(hours & 0x7F);   /* Mask 12/24 bit */
        day     = bcd_to_bin(day);
        month   = bcd_to_bin(month);
        year    = bcd_to_bin(year);
        century = bcd_to_bin(century);
    }

    /* Handle 12-hour mode */
    if (!(regB & 0x02) && (hours & 0x80)) {
        hours = ((hours & 0x7F) + 12) % 24;
    }

    current_seconds = seconds;
    current_minutes = minutes;
    current_hours   = hours;
    current_day     = day;
    current_month   = month;

    if (century != 0)
        current_year = century * 100 + year;
    else
        current_year = 2000 + year;
}

/* ──────── Public API ──────── */
void clock_init(void) {
    /* Configure PIT Channel 0 for 1000 Hz square wave */
    uint16_t divisor = PIT_FREQUENCY / TARGET_HZ;
    outb(PIT_COMMAND, 0x36);                    /* Chan 0, lobyte/hibyte, mode 3 */
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));

    /* Register IRQ0 handler */
    register_interrupt_handler(IRQ0, pit_irq_handler);

    /* Read the real-time clock */
    rtc_read();

    /* Record boot tick */
    boot_time = system_ticks;
}

void sleep_ms(uint32_t ms) {
    uint32_t target = system_ticks + ms;
    while (system_ticks < target)
        hlt();   /* Let PIT interrupt wake us */
}

void display_clock(void) {
    char num_str[10];

    num_to_str(num_str, current_hours, 2);
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring(num_str);
    terminal_writestring(":");
    num_to_str(num_str, current_minutes, 2);
    terminal_writestring(num_str);
    terminal_writestring(":");
    num_to_str(num_str, current_seconds, 2);
    terminal_writestring(num_str);
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
}

void display_date(void) {
    char num_str[10];

    num_to_str(num_str, current_year, 4);
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring(num_str);
    terminal_writestring("-");
    num_to_str(num_str, current_month, 2);
    terminal_writestring(num_str);
    terminal_writestring("-");
    num_to_str(num_str, current_day, 2);
    terminal_writestring(num_str);
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
}

void display_uptime(void) {
    uint32_t uptime_ms = system_ticks - boot_time;
    uint32_t uptime_seconds = uptime_ms / 1000;
    uint32_t days    = uptime_seconds / 86400;
    uint32_t hours   = (uptime_seconds % 86400) / 3600;
    uint32_t minutes = (uptime_seconds % 3600) / 60;
    uint32_t secs    = uptime_seconds % 60;

    char num_str[10];

    if (days > 0) {
        num_to_str(num_str, days, 0);
        terminal_writestring(num_str);
        terminal_writestring(" day");
        if (days != 1) terminal_writestring("s");
        terminal_writestring(", ");
    }

    num_to_str(num_str, hours, 2);
    terminal_writestring(num_str);
    terminal_writestring(":");
    num_to_str(num_str, minutes, 2);
    terminal_writestring(num_str);
    terminal_writestring(":");
    num_to_str(num_str, secs, 2);
    terminal_writestring(num_str);
}
