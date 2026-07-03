/*
 * ArcadeOS – PC speaker tone generator
 *
 * PIT channel 2 drives the speaker gate on port 0x61 with a square wave.
 * Cheap, driver-free, works on essentially every PC (and in QEMU with
 * -machine pcspk-audiodev=...). The tone is stopped from the PIT tick
 * handler once its duration elapses.
 */

#include "audio.h"
#include "clock.h"

#define PIT_CH2      0x42
#define PIT_CMD      0x43
#define SPEAKER_PORT 0x61
#define PIT_HZ       1193182u

static volatile uint32_t beep_end = 0;
static volatile int      beeping  = 0;

void pcspk_tone(uint32_t freq_hz, uint32_t ms) {
    if (freq_hz < 20 || freq_hz > 20000) {
        pcspk_stop();
        return;
    }

    uint32_t divisor = PIT_HZ / freq_hz;
    outb(PIT_CMD, 0xB6);                       /* Channel 2, lo/hi, mode 3 */
    outb(PIT_CH2, (uint8_t)(divisor & 0xFF));
    outb(PIT_CH2, (uint8_t)((divisor >> 8) & 0xFF));

    outb(SPEAKER_PORT, inb(SPEAKER_PORT) | 0x03);   /* Gate + data on */
    beep_end = system_ticks + ms;
    beeping  = 1;
}

void pcspk_stop(void) {
    outb(SPEAKER_PORT, inb(SPEAKER_PORT) & (uint8_t)~0x03);
    beeping = 0;
}

void pcspk_tick(void) {
    if (beeping && system_ticks >= beep_end)
        pcspk_stop();
}
