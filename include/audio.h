#ifndef AUDIO_H
#define AUDIO_H

#include "types.h"

/*
 * ArcadeOS – audio subsystem
 *
 * One simple capability, two backends: play a square-wave tone for a
 * fixed duration. Routed to the AC97 PCM DAC when an AC97 codec exists
 * (48 kHz synthesized square wave, real speakers/headphones), falling
 * back to the PC speaker (PIT channel 2 gate) otherwise. Backs SYS_SOUND.
 */

/* Probe backends (AC97 via PCI; PC speaker always works). */
void audio_init(void);

/* Play a square wave: freq in Hz, duration in ms. freq == 0 stops any
 * playing tone. Duration is capped by the backend's buffer budget
 * (~680 ms on AC97). Starting a new tone replaces the current one. */
void audio_tone(uint32_t freq_hz, uint32_t ms);

/* Called from the PIT tick: auto-stops the PC speaker gate. */
void audio_tick(void);

/* ──────── Backend APIs (used by audio.c) ──────── */

/* PC speaker (pcspk.c) */
void pcspk_tone(uint32_t freq_hz, uint32_t ms);
void pcspk_stop(void);
void pcspk_tick(void);

/* AC97 codec (ac97.c) */
int  ac97_init(void);
int  ac97_is_present(void);
void ac97_tone(uint32_t freq_hz, uint32_t ms);
void ac97_stop(void);
void ac97_tick(void);

#endif /* AUDIO_H */
