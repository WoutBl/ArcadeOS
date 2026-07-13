#ifndef CONSOLE_ABI_H
#define CONSOLE_ABI_H

/*
 * ArcadeOS – Kernel/User Shared Console ABI
 *
 * Structures and constants passed across the int 0x80 boundary for the
 * graphics and controller syscalls. Included by BOTH the kernel and
 * libc, so keep it freestanding (fixed-width types only).
 */

#include <stdint.h>

/* ──────── SYS_GFX_INFO ──────── */
typedef struct {
    uint32_t width;      /* Pixels */
    uint32_t height;     /* Pixels */
    uint32_t pitch;      /* Bytes per scanline of the USER buffer (width*4) */
    uint32_t bpp;        /* Always 32 */
} gfx_info_t;

/* ──────── SYS_PAD_READ ──────── */

/* Button bitmask (buttons field) */
#define PAD_BTN_A       (1 << 0)
#define PAD_BTN_B       (1 << 1)
#define PAD_BTN_X       (1 << 2)
#define PAD_BTN_Y       (1 << 3)
#define PAD_BTN_UP      (1 << 4)
#define PAD_BTN_DOWN    (1 << 5)
#define PAD_BTN_LEFT    (1 << 6)
#define PAD_BTN_RIGHT   (1 << 7)
#define PAD_BTN_START   (1 << 8)
#define PAD_BTN_SELECT  (1 << 9)
#define PAD_BTN_L1      (1 << 10)
#define PAD_BTN_R1      (1 << 11)
#define PAD_BTN_L3      (1 << 12)
#define PAD_BTN_R3      (1 << 13)

#define PAD_MAX_CONTROLLERS 4

typedef struct {
    uint8_t  connected;   /* 1 if a controller is present at this index */
    uint8_t  source;      /* PAD_SOURCE_* */
    uint16_t buttons;     /* PAD_BTN_* bitmask */
    int16_t  lx, ly;      /* Left stick,  -32768..32767 (up = negative Y) */
    int16_t  rx, ry;      /* Right stick, -32768..32767 */
    uint8_t  lt, rt;      /* Analog triggers, 0..255 */
} pad_state_t;

#define PAD_SOURCE_NONE     0
#define PAD_SOURCE_KEYBOARD 1   /* Keyboard-mapped virtual pad */
#define PAD_SOURCE_USB      2   /* USB HID gamepad */

/* ──────── SYS_READDIR ──────── */
typedef struct {
    char     name[64];
    uint32_t flags;      /* VFS_FLAG_* (1=file, 2=dir, 4=device) */
    uint32_t size;       /* Bytes (0 for dirs/devices) */
} dirent_info_t;

/* ──────── SYS_SOUND_EX ──────── */

#define SOUND_VOICES     4      /* Mixer voices (kernel MIX_VOICES) */
#define SOUND_PCM_MAX    16384  /* Max samples per PCM upload */

#define SOUND_OP_STOP    0      /* Stop the voice */
#define SOUND_OP_SQUARE  1      /* Square-wave tone */
#define SOUND_OP_PCM     2      /* 16-bit mono PCM clip (copied) */

typedef struct {
    uint32_t voice;        /* 0..SOUND_VOICES-1 */
    uint32_t op;           /* SOUND_OP_* */
    uint32_t vol;          /* 0..255 */
    uint32_t freq_hz;      /* SQUARE: tone frequency */
    uint32_t dur_ms;       /* SQUARE: duration */
    uint32_t sample_rate;  /* PCM: 4000..48000 Hz */
    uint32_t sample_count; /* PCM: samples (<= SOUND_PCM_MAX) */
    uint64_t sample_ptr;   /* PCM: int16_t* in the caller's space */
} sound_req_t;

/* ──────── SYS_NET (UDP netplay) ──────── */

#define NET_OP_INFO 0   /* Returns the console's IPv4 (0 = no NIC) */
#define NET_OP_BIND 1   /* Bind the game's UDP socket to rq->port */
#define NET_OP_SEND 2   /* Datagram to rq->ip:rq->port; -2 = ARP pending, retry */
#define NET_OP_RECV 3   /* Dequeue one datagram; fills ip/port/len */

#define NET_MSG_MAX 512

typedef struct {
    uint32_t op;     /* NET_OP_* */
    uint32_t ip;     /* Host-order IPv4 (10.0.2.2 = 0x0A000202) */
    uint32_t port;
    uint32_t len;    /* SEND: payload bytes; RECV: buffer capacity */
    uint64_t buf;    /* Payload pointer in the caller's space */
} net_req_t;

#endif /* CONSOLE_ABI_H */
