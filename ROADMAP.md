# ArcadeOS Roadmap

My plan for where this goes next. I've got a bare-metal x86 console booting
via GRUB multiboot into a 640x480x32 framebuffer, a Ring-3 launcher + ELF
games (Pong/Snake/Breakout), UHCI USB (keyboard + DualShock 4 merged into pad
0), and FAT32-over-ATA-PIO save data — all only ever run in QEMU so far. No
audio, no networking, xHCI is a stub, no real hardware testing yet.

## Up next

1. ~~**Write my own bootloader.**~~ **DONE (July 2026).** Two-stage BIOS
   bootloader (`boot/stage1.asm` + `boot/stage2.asm`) embedded in the FAT32
   volume's reserved sectors — GRUB and the ISO are gone entirely, one
   self-booting `arcadeos.img`. Stage 2 does A20, E820, VBE mode-set, and
   loads the kernel flat binary to 1 MiB.

2. ~~**Move off 32-bit, onto 64-bit (long mode).**~~ **DONE (July 2026).**
   The whole stack is 64-bit now: stage 2 builds identity page tables and
   enters long mode before jumping to the kernel; the kernel has a 64-bit
   GDT/IDT/TSS, 4-level paging (2 MiB kernel identity pages + 4 KiB user
   overlays), 64-bit context switching, and loads ELF64 games entered at
   main(argc, argv) per the SysV ABI. Toolchain switched to x86_64-elf-gcc
   with -mno-red-zone/-mno-sse for the kernel. Verified in QEMU: launcher,
   Snake, Breakout, high-score save to FAT32, crash-safe quit-to-launcher.

3. ~~**AHCI driver.**~~ **DONE (July 2026).** Polled AHCI driver
   (`src/ahci.c`) + a disk dispatch layer (`src/disk.c`) that prefers AHCI
   and falls back to the legacy ATA PIO driver. QEMU now attaches the game
   volume over AHCI by default (`make run`); `make run-ide` keeps the PIO
   path honest. This was the real-hardware blocker for storage.

4. ~~**Logging, done properly.**~~ **DONE (July 2026) — file + serial
   half.** Every character that reaches the serial mirror also lands in a
   32 KiB ring buffer that the idle task flushes to `KERNEL.LOG` on the
   game volume every ~2 s — so real hardware keeps a boot log on disk even
   with no serial cable attached. Still to do later: the REST/TCP log
   streaming endpoint, which needs the NIC driver first (see networking
   below).

## Also on the list

- ~~**Audio.**~~ **DONE (July 2026) — first pass.** `SYS_SOUND` syscall
  (freq + duration square-wave tones) with two backends: AC97 PCM out
  (48 kHz synthesized, `src/ac97.c`) preferred, PC speaker (PIT channel 2,
  `src/pcspk.c`) fallback. All three games + the launcher have sound
  effects now. Verified headless by capturing QEMU audio to WAV and
  checking the spectrum. Still to do later: Intel HDA backend for real
  motherboards, and PCM sample playback / mixing as part of the SDK work
  (tones only for now).

- **Graphics/GPU.** What I have today already renders through the
  framebuffer the graphics card exposes (VBE/LFB) — it's just unaccelerated
  software rendering. A real vendor 2D/3D driver (Intel/AMD/Nvidia) is not
  realistic for a hobby OS; the better use of time is double-buffered page
  flipping and not hardcoding a single 640x480 mode.

- **Game engine + SDK.** Formalize sprite/entity management, a fixed
  timestep loop helper, and input/save helpers into a real internal library
  instead of every game hand-rolling raw syscalls. Package it as something
  I can actually hand to myself (or someone else) as an SDK with a template
  for starting a new game.

- **PC-side game editor / dev tool (far future, sketchy idea).** Build an
  editor that runs on a normal PC (not on ArcadeOS itself) for putting
  together a game — levels, sprites, whatever — using the SDK above, then
  export it onto removable media (SD card or similar) and have the console
  pick it up the way it already lists `*.ELF` files off the FAT32 volume.
  Not sure yet exactly how the console-side detection/mounting would work
  (whether it's a second FAT volume, a USB mass-storage read, etc.) — this
  needs its own design pass once the SDK itself exists.

- **New game.** Once the SDK exists, build something that actually exercises
  analog sticks and multiple pads, not just a single-screen single-pad game
  like the current three — a twin-stick shooter or a rhythm game (which also
  gives audio a reason to exist) are the strongest candidates.

- **Launcher polish.** Box art, categories, recently-played — worth doing
  once there are more than 3-4 games to look at.

- **Networking (stretch).** A NIC driver (rtl8139 in QEMU to start) mainly
  to unlock the remote-logging item above and, longer-term, shared
  high-score sync. Comes after the core stuff above is solid.

## Not right now

- **Real hardware bring-up** (flashing an actual PC, checking keyboard,
  controller, screen, audio all work outside QEMU). I know this is on the
  list eventually — boot media, PS/2-vs-USB-only keyboards, DS4 over
  UHCI-only ports, VBE mode fallback, no-serial debugging on real hardware —
  but I'm parking it for later. Most of the "up next" items (bootloader,
  64-bit, AHCI) are exactly the prerequisites that make this worth
  attempting when I do get to it.
