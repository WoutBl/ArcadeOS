/*
 * ArcadeOS – audio routing + 4-voice software mixer (see audio.h)
 *
 * The mixer is the single source of samples: the AC97 transport calls
 * mixer_render() to fill ring pages just ahead of the hardware. Voices
 * are square-wave tones or kernel-copied 16-bit mono PCM clips.
 *
 * Concurrency: voice state is updated from syscall context (interrupts
 * off — int 0x80 is an interrupt gate) and read from the PIT tick, on a
 * single CPU. Updates briefly cli() so a tick can never see a voice
 * half-programmed.
 */

#include "audio.h"
#include "vga.h"
#include "heap.h"

#define SAMPLE_RATE 48000

typedef struct {
    int      active;
    int      is_pcm;
    int16_t  amp;            /* Peak amplitude after volume scaling */
    /* Square voice */
    uint32_t phase;          /* 8.24 fixed-point cycle position */
    uint32_t phase_inc;
    uint32_t frames_left;
    /* PCM voice */
    int16_t* data;           /* Kernel copy (kmalloc, reused per voice) */
    uint32_t len;            /* Samples */
    uint32_t pos_fx;         /* 20.12 fixed-point sample position */
    uint32_t step_fx;        /* rate/48000 in 20.12 */
} mix_voice_t;

static mix_voice_t voices[MIX_VOICES];

static inline uint64_t irq_save(void) {
    uint64_t f;
    asm volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void irq_restore(uint64_t f) {
    if (f & 0x200) asm volatile("sti");
}

void audio_init(void) {
    if (!ac97_init()) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
        terminal_writestring("[AUDIO] No AC97 codec - PC speaker fallback\n");
    }
}

/* ──────── Voice control ──────── */

void audio_stop_voice(int v) {
    uint64_t f = irq_save();
    if (v < 0) {
        for (int i = 0; i < MIX_VOICES; i++) voices[i].active = 0;
    } else if (v < MIX_VOICES) {
        voices[v].active = 0;
    }
    irq_restore(f);
}

int audio_tone_voice(int v, uint32_t freq_hz, uint32_t ms, uint8_t vol) {
    if (v < 0 || v >= MIX_VOICES) return -1;
    if (freq_hz < 20 || freq_hz > 20000) { audio_stop_voice(v); return 0; }
    if (ms > 5000) ms = 5000;

    uint64_t f = irq_save();
    mix_voice_t* vc = &voices[v];
    vc->is_pcm      = 0;
    vc->amp         = (int16_t)((9000 * (int)vol) / 255);
    vc->phase       = 0;
    vc->phase_inc   = (uint32_t)(((uint64_t)freq_hz << 24) / SAMPLE_RATE);
    vc->frames_left = (SAMPLE_RATE / 1000) * ms;
    vc->active      = 1;
    irq_restore(f);
    return 0;
}

int audio_pcm_play(int v, const int16_t* data, uint32_t nsamples,
                   uint32_t rate, uint8_t vol) {
    if (v < 0 || v >= MIX_VOICES) return -1;
    if (!data || nsamples == 0 || nsamples > MIX_PCM_MAX_SAMPLES) return -1;
    if (rate < 4000 || rate > 48000) return -1;

    mix_voice_t* vc = &voices[v];

    /* (Re)allocate the voice's kernel buffer and copy the clip while
     * the voice is off, so a tick never reads a half-copied buffer. */
    uint64_t f = irq_save();
    vc->active = 0;
    irq_restore(f);

    if (!vc->data) {
        vc->data = (int16_t*)kmalloc(MIX_PCM_MAX_SAMPLES * sizeof(int16_t));
        if (!vc->data) return -1;
    }
    memcpy(vc->data, data, nsamples * sizeof(int16_t));

    f = irq_save();
    vc->is_pcm  = 1;
    vc->amp     = (int16_t)vol;                 /* Used as a /255 scaler */
    vc->len     = nsamples;
    vc->pos_fx  = 0;
    vc->step_fx = (uint32_t)(((uint64_t)rate << 12) / SAMPLE_RATE);
    vc->active  = 1;
    irq_restore(f);
    return 0;
}

/* Legacy single-tone API: voice 0, full volume; 0 Hz = stop everything */
void audio_tone(uint32_t freq_hz, uint32_t ms) {
    if (freq_hz == 0) {
        audio_stop_voice(-1);
        pcspk_stop();
        return;
    }
    if (ac97_is_present())
        audio_tone_voice(0, freq_hz, ms, 255);
    else
        pcspk_tone(freq_hz, ms);
}

/* ──────── The mixer ──────── */

void mixer_render(int16_t* out, uint32_t frames) {
    for (uint32_t i = 0; i < frames; i++) {
        int32_t acc = 0;

        for (int v = 0; v < MIX_VOICES; v++) {
            mix_voice_t* vc = &voices[v];
            if (!vc->active) continue;

            if (vc->is_pcm) {
                uint32_t idx = vc->pos_fx >> 12;
                if (idx >= vc->len) { vc->active = 0; continue; }
                acc += ((int32_t)vc->data[idx] * vc->amp) / 255;
                vc->pos_fx += vc->step_fx;
            } else {
                if (vc->frames_left == 0) { vc->active = 0; continue; }
                acc += (vc->phase & 0x00800000) ? -vc->amp : vc->amp;
                vc->phase += vc->phase_inc;
                vc->frames_left--;
            }
        }

        if (acc >  32767) acc =  32767;
        if (acc < -32768) acc = -32768;
        out[i * 2]     = (int16_t)acc;   /* Left */
        out[i * 2 + 1] = (int16_t)acc;   /* Right */
    }
}

/* ──────── Boot chime (and permanent PCM self-test) ──────── */

/* ~0.25 s clip at 24 kHz: a rising two-note triangle-wave arpeggio with
 * a linear fade — all integer math, no tables. */
#define CHIME_RATE  24000
#define CHIME_LEN   6000

static int16_t chime_buf[CHIME_LEN];

void audio_boot_chime(void) {
    if (!ac97_is_present()) return;

    for (uint32_t i = 0; i < CHIME_LEN; i++) {
        /* Two notes: A5 (880 Hz) then E6 (1319 Hz) */
        uint32_t freq = (i < CHIME_LEN / 2) ? 880 : 1319;
        uint32_t period = CHIME_RATE / freq;
        uint32_t ph = i % period;
        /* Triangle: 0..period/2 rises, then falls */
        int32_t tri = (ph < period / 2)
                    ? (int32_t)(ph * 4000 / (period / 2)) - 2000
                    : 2000 - (int32_t)((ph - period / 2) * 4000 / (period / 2));
        /* Linear fade-out over the whole clip */
        int32_t env = (int32_t)(CHIME_LEN - i);
        chime_buf[i] = (int16_t)(tri * 8 * env / CHIME_LEN);
    }
    audio_pcm_play(MIX_VOICES - 1, chime_buf, CHIME_LEN, CHIME_RATE, 200);
}

void audio_tick(void) {
    pcspk_tick();
    ac97_tick();
}
