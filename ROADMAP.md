# ArcadeOS Roadmap

Current state: bare-metal x86, GRUB multiboot into a 640x480x32 LFB, Ring-3
launcher + ELF games (Pong/Snake/Breakout), UHCI USB (keyboard + DualShock 4
merged into pad 0), FAT32-over-ATA-PIO save data, all validated in QEMU
(`make run` / `make run-headless`). No audio, no networking, xHCI is a stub,
never booted on real hardware.

## Tier 0 — Real hardware bring-up

Everything so far has only run in QEMU. Flashing a real PC is its own project
because several subsystems were only ever exercised against emulated devices.

- **Bootable media** — `dd`/Rufus the `.iso` to a USB stick. GRUB multiboot
  needs legacy BIOS (or CSM) boot; most modern PCs default to UEFI-only, so
  this may require picking older/BIOS-mode hardware or adding UEFI boot
  support (GRUB can chainload multiboot under UEFI, but it's untested here).
- **Keyboard** — `keyboard.c` drives PS/2 IRQ1; confirm this still works on
  a PC with only USB keyboards (no PS/2 port) — may need USB HID boot
  keyboard support in `usb.c`, not just the DS4 report parser.
- **Controller** — DS4 currently only validated via QEMU `-device usb-host`
  with `sudo` (macOS HID driver seizure workaround). Real hardware won't have
  that host-OS conflict, but UHCI-only support means USB3-only ports/hubs may
  not enumerate the pad at all — ties into the xHCI item below.
- **Screen** — `fb.c` requests a fixed 640x480x32 mode via multiboot video
  fields. Real graphics cards won't all honor that mode/resolution the same
  way QEMU's stdvga does; need a fallback path (query available VBE modes,
  pick nearest match) instead of assuming the request always succeeds.
- **Audio** — depends on Tier 1 existing at all; then needs a driver for
  whatever's actually on the board (see GPU/audio hardware notes below).
- **Disk** — `ata.c` is legacy PATA PIO. Most PCs built in the last ~15 years
  expose disks via AHCI, not legacy IDE, even with CSM/legacy boot enabled.
  This likely needs a real AHCI driver before save data works on real
  hardware at all (see Tier 4).
- **No serial console on real hardware** — `serial.log`-based debugging goes
  away without a debug UART/cable. Worth an on-screen panic/debug overlay
  (crash reason, register dump) since there's no headless fallback once it's
  not running under QEMU.

## Tier 1 — Audio

Biggest missing subsystem overall, and a prerequisite for a lot of "feels
like a real console" work (rhythm games, feedback SFX, etc).

- PC speaker square-wave beeps first (cheap, no new hardware driver, works
  in QEMU and on basically any real PC).
- Real backend next: AC97 (older, simpler, well-supported in QEMU) vs. Intel
  HDA (what most real motherboards built after ~2005 actually have — needed
  for Tier 0 real-hardware audio to work at all). Likely want both: AC97 for
  QEMU dev loop, HDA for real hardware.
- `SYS_AUDIO_PLAY`-style syscall + small mixer so games can layer SFX over
  music without hand-rolling PCM mixing per game.

## Tier 2 — Graphics / GPU

- Today's renderer is a full software blit onto a VBE/multiboot linear
  framebuffer — that *is* "using the graphics card," just in its dumbest
  mode (no 2D/3D acceleration, no vendor driver). A real Intel/AMD/Nvidia
  accelerated driver is out of scope for a hobby OS (thousands of hours of
  vendor-specific reverse engineering); the realistic path is staying on
  VBE/VESA (or generic UEFI GOP on real hardware) and optimizing the
  *software* path instead.
- Double-buffered page flipping instead of a full blit on every
  `gfx_present_buffer()` call, to fix tearing/timing under real display
  refresh rates (not just QEMU TCG frame pacing).
- Mode negotiation (list available VBE modes, don't hardcode 640x480) — ties
  directly into the Tier 0 real-hardware screen item.

## Tier 3 — Game engine + SDK

Right now games are ELFs linked against `libc/console.h` calling raw
syscalls directly. Worth formalizing into a real internal engine + public
SDK so new games (yours or others') don't each reinvent sprite/input/save
boilerplate:

- Engine layer: sprite/entity management, fixed-timestep game loop helper,
  input abstraction over `pad_state_t` (edge/hold/repeat helpers), save-slot
  helpers on top of `SYS_SAVE`/`SYS_LOAD`.
- SDK packaging: a documented header + static lib (`libarcade.a`?) separate
  from the OS-internal `libc/`, plus a "new game" template/skeleton
  (Makefile target, minimal main loop) so starting a new title is
  copy-a-template rather than reading existing games' source.
- Once the SDK exists, DS4 rumble output (per the note below) and audio
  (Tier 1) should be exposed through it, not just raw syscalls.
- DualShock 4 rumble/LED output — input decoding already exists in
  `src/usb.c`; adding the output report is a small, high-payoff extension of
  code that already works.

## Tier 4 — Storage / save data

- **AHCI/SATA driver** — needed for real hardware (see Tier 0); `ata.c`
  legacy PIO likely won't see most real disks at all.
- Save game state to whatever's actually connected on real hardware (SSD/HDD
  via AHCI) instead of only the emulated FAT32 game volume — may mean a
  separate writable partition/volume distinct from the read-mostly game
  volume the launcher boots from.
- Multiple save slots per game, directory-based instead of one flat 8.3 file
  per title (current limit: whole-file, 64 KiB max, single slot).
- Settings persistence (theme, control remapping) using the same save path,
  since `SYS_THEME` exists but nothing currently persists it across reboots.

## Tier 5 — New game

A good next title should stress the engine/SDK (Tier 3) harder than
Pong/Snake/Breakout do — those are all single-screen, single-pad, no-audio.
Options, roughly in order of how much they'd exercise new subsystems:

- **Twin-stick shooter** — exercises analog stick input (DS4 sticks already
  decoded but unused by existing games), multiple simultaneous sprites, and
  is a natural first user of the audio SFX API.
- **Simple rhythm game** — directly motivates and validates the audio
  subsystem (Tier 1) rather than treating sound as an afterthought.
- **Local 2-player mode** (Pong or a new title) — pad 0-3 merging already
  exists in `gamepad.c` but has never been exercised by an actual 2-controller
  game.

## Tier 6 — Launcher / UX polish

- Box-art thumbnails (sprite rendering already supports alpha-keyed images),
  categories/sorting, "recently played" — more valuable once there are 4-5+
  titles instead of 3.

## Tier 7 — Stretch: networking

- Minimal NIC driver (rtl8139 is well-supported in QEMU) purely for a shared
  high-score/leaderboard sync. Large scope jump relative to everything else
  here — only worth it once the console-like core (audio, real hardware,
  SDK) is solid.

## Suggested order

Tier 0 (real hardware) and Tier 1 (audio) are the two biggest gaps and don't
block each other — could run in parallel. Tier 3 (engine/SDK) is worth doing
before Tier 5 (new game) so the new title is built on the SDK instead of
copy-pasting an existing game's raw-syscall style. Tier 4's AHCI driver is a
hard dependency of "save to real disk" specifically, not of Tier 0 broadly
(QEMU's `run`/`run-headless` targets keep working on `ata.c` either way).
