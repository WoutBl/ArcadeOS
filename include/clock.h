#ifndef CLOCK_H
#define CLOCK_H

#include "types.h"

/* PIT (Programmable Interval Timer) constants */
#define PIT_CHANNEL0 0x40
#define PIT_COMMAND  0x43
#define PIT_FREQUENCY 1193182   /* Base oscillator frequency in Hz */
#define TARGET_HZ     1000     /* Desired tick rate */

/* CMOS / RTC ports */
#define CMOS_ADDRESS 0x70
#define CMOS_DATA    0x71

/* Time state */
extern volatile uint32_t system_ticks;  /* Incremented by IRQ0, 1 tick = 1 ms */
extern uint32_t current_seconds;
extern uint32_t current_minutes;
extern uint32_t current_hours;
extern uint32_t current_day;
extern uint32_t current_month;
extern uint32_t current_year;
extern uint32_t boot_time;

/* Public API */
void clock_init(void);          /* Configure PIT + read RTC */
void sleep_ms(uint32_t ms);     /* Busy-wait using PIT ticks */
void display_clock(void);
void display_date(void);
void display_uptime(void);

#endif /* CLOCK_H */
