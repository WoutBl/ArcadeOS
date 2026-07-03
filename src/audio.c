/*
 * ArcadeOS – audio routing (see audio.h)
 */

#include "audio.h"
#include "vga.h"

void audio_init(void) {
    if (!ac97_init()) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
        terminal_writestring("[AUDIO] No AC97 codec - PC speaker fallback\n");
    }
}

void audio_tone(uint32_t freq_hz, uint32_t ms) {
    if (freq_hz == 0) {
        ac97_stop();
        pcspk_stop();
        return;
    }
    if (ac97_is_present())
        ac97_tone(freq_hz, ms);
    else
        pcspk_tone(freq_hz, ms);
}

void audio_tick(void) {
    pcspk_tick();
    ac97_tick();
}
