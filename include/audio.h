#ifndef AUDIO_H
#define AUDIO_H

#include "types.h"

/*
 * ArcadeOS – audio subsystem
 *
 * A 4-voice software mixer feeding an always-running 48 kHz AC97 ring
 * (real speakers/headphones), with a PC-speaker fallback for the legacy
 * single-tone path. Voices play either square-wave tones or uploaded
 * 16-bit mono PCM samples (any rate, resampled by the mixer). Backs
 * SYS_SOUND (legacy, voice 0) and SYS_SOUND_EX.
 */

#define MIX_VOICES          4
#define MIX_PCM_MAX_SAMPLES 16384   /* Per-voice PCM cap (32 KiB) */

/* Probe backends (AC97 via PCI; PC speaker always works). */
void audio_init(void);

/* Legacy: square wave on voice 0. freq == 0 stops ALL voices. Duration
 * beyond ~5 s is capped. This is the only call the PC speaker fallback
 * can honor. */
void audio_tone(uint32_t freq_hz, uint32_t ms);

/* Square wave on a specific voice. vol 0-255. Returns 0/-1. */
int audio_tone_voice(int v, uint32_t freq_hz, uint32_t ms, uint8_t vol);

/* Play 16-bit mono PCM on a voice. The data is COPIED into a kernel
 * buffer (max MIX_PCM_MAX_SAMPLES), so the caller's memory is free to
 * go away. rate 4000-48000 Hz, resampled to 48 kHz. Returns 0/-1. */
int audio_pcm_play(int v, const int16_t* data, uint32_t nsamples,
                   uint32_t rate, uint8_t vol);

/* Stop one voice (v == -1: all voices). */
void audio_stop_voice(int v);

/* Render the current mix: 'frames' stereo 16-bit frames at 48 kHz.
 * Called by the AC97 transport just ahead of the hardware position. */
void mixer_render(int16_t* out, uint32_t frames);

/* Synthesized PCM boot chime — also serves as a built-in self-test of
 * the PCM path on every boot. */
void audio_boot_chime(void);

/* Called from the PIT tick: refills the AC97 ring, ticks the speaker. */
void audio_tick(void);

/* ──────── Backend APIs (used by audio.c) ──────── */

/* PC speaker (pcspk.c) */
void pcspk_tone(uint32_t freq_hz, uint32_t ms);
void pcspk_stop(void);
void pcspk_tick(void);

/* AC97 transport (ac97.c): always-running ring, mixer-rendered */
int  ac97_init(void);
int  ac97_is_present(void);
void ac97_tick(void);

/* Intel HDA transport (hda.c): same model, what real hardware has */
int  hda_init(void);
int  hda_is_present(void);
void hda_tick(void);

#endif /* AUDIO_H */
