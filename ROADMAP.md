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

2. **Move off 32-bit, onto 64-bit (long mode).** I don't think plain 32-bit
   is a safe bet for modern PCs going forward, so I want the kernel running
   in long mode. (Worth noting for myself: x86-64 CPUs still execute 32-bit
   protected mode in hardware just fine — the real risk to plain 32-bit on
   real hardware is newer boards going UEFI-only with no BIOS/CSM fallback,
   which is more about the bootloader than the CPU mode. Doesn't change the
   plan, just means the bootloader rewrite and the 64-bit move are really
   the same piece of work: new GDT/segments, 4-level paging instead of the
   current 2-level scheme, toolchain switch to x86_64-elf-gcc, ELF64
   loading, and a syscall ABI update since arg-passing registers change.)

3. **AHCI driver.** `ata.c` today is legacy PATA PIO, which won't see disks
   on real hardware at all. Need this before storage/save-data work means
   anything outside QEMU.

4. **Logging, done properly.** Always write logs to both a file on disk
   *and* serial — not one or the other, always both, so I never lose a boot
   log because I forgot to redirect the right thing. Later, once there's a
   NIC driver (see networking below), add a REST/TCP endpoint so I can
   stream logs over Ethernet/Wi-Fi instead of pulling a file off disk or
   tailing a serial cable.

## Also on the list

- **Audio.** Still the biggest missing subsystem for anything that's
  supposed to feel like a console. PC speaker beep first (cheap, works
  everywhere), then a real backend — AC97 for QEMU, Intel HDA for whatever
  actually ends up on real hardware.

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
