# ArcadeOS Roadmap

My plan for where this goes next. Current state: a bare-metal **x86-64**
console with its own two-stage BIOS bootloader (no GRUB), booting into a
640x480x32 VBE framebuffer, running a Ring-3 launcher + four SDK-based
games off a self-booting FAT32 volume attached via AHCI, with sound,
on-disk kernel logging, and save data. All validated in QEMU; real
hardware is still on the list.

## Done

1. **Own bootloader** (July 2026). Two-stage BIOS bootloader
   (`boot/stage1.asm` + `boot/stage2.asm`) embedded in the FAT32 volume's
   reserved sectors — GRUB and the ISO are gone entirely, one self-booting
   `arcadeos.img`. Stage 2 does A20, E820, VBE mode-set, and loads the
   kernel flat binary to 1 MiB.

2. **64-bit port** (July 2026). The whole stack runs in long mode: stage 2
   builds identity page tables and enters long mode before jumping to the
   kernel; the kernel has a 64-bit GDT/IDT/TSS, 4-level paging (2 MiB
   kernel identity pages + 4 KiB user overlays), 64-bit context switching,
   and loads ELF64 games entered at main(argc, argv) per the SysV ABI.
   Toolchain: x86_64-elf-gcc with -mno-red-zone/-mno-sse.

3. **AHCI driver** (July 2026). Polled AHCI driver (`src/ahci.c`) + a disk
   dispatch layer (`src/disk.c`) that prefers AHCI and falls back to the
   legacy ATA PIO driver. QEMU attaches the game volume over AHCI by
   default; `make run-ide` keeps the PIO path honest. This was the
   real-hardware blocker for storage.

4. **Logging — file + serial half** (July 2026). Every character that
   reaches the serial mirror also lands in a 32 KiB ring buffer that the
   idle task flushes to `KERNEL.LOG` on the game volume every ~2 s, so
   real hardware keeps a boot log on disk even without a serial cable.
   (The REST/TCP streaming half still needs the NIC driver — see
   networking below.)

5. **Audio — first pass** (July 2026). `SYS_SOUND` square-wave tones with
   two backends: AC97 PCM out on an always-running 48 kHz ring
   (`src/ac97.c`), PC speaker (`src/pcspk.c`) fallback. All games + the
   launcher have sound effects. (Intel HDA backend and PCM
   sample/mixing still to come.)

6. **Game engine + SDK — first version** (July 2026). `libarcade`
   (sdk/arcade.h): fixed-timestep loop, input edge detection, fixed-point
   math, color-keyed sprites, entities with AABB collision, save slots,
   canned SFX, PRNG — documented in sdk/README.md with a template.
   STARCATCH (apps/starcatch.c) is the reference game, and Pong, Snake,
   Breakout, and the launcher are all ported onto the SDK (no game talks
   raw syscalls anymore).

7. **Local 2-player mode** (July 2026). The keyboard is split into two
   virtual pads in the kernel (pad 0 = arrows/XZCV/Enter/Tab, pad 1 =
   WASD + R/T/F/G), the SDK exposes player 2 as `a.pad2` with the same
   edge detection as player 1, and Pong has a 2P mode (Y toggles the AI
   off; player 2 drives the right paddle with W/S). A USB pad still
   merges into pad 0, so DS4-vs-keyboard works out of the box.

## Up next

- **Graphics/GPU.** What I have today already renders through the
  framebuffer the graphics card exposes (VBE/LFB) — it's just unaccelerated
  software rendering. A real vendor 2D/3D driver (Intel/AMD/Nvidia) is not
  realistic for a hobby OS; the better use of time is double-buffered page
  flipping and not hardcoding a single 640x480 mode.

- **New game.** Something that actually exercises analog sticks and
  multiple pads — a twin-stick shooter or a rhythm game (which gives audio
  a reason to grow) are the strongest candidates. Build it on the SDK.

- **Launcher polish.** Pretty display names (the 8.3 filename limit makes
  STARCATCH show as STARCATC.ELF), box art, categories, recently-played —
  worth doing now that there are 4+ games.

- **Networking (stretch).** A NIC driver (rtl8139 in QEMU to start) mainly
  to unlock the remote-logging endpoint and, longer-term, shared
  high-score sync. Comes after the core stuff above is solid.

- **PC-side game editor / dev tool (far future, just an idea).** Build an
  editor that runs on a normal PC (not on ArcadeOS itself) for putting
  together a game — levels, sprites, whatever — using the SDK, then
  export it onto removable media (SD card or similar) and have the console
  pick it up the way it already lists `*.ELF` files off the FAT32 volume.
  Needs its own design pass.



## Not right now

- **Real hardware bring-up** (flashing an actual PC, checking keyboard,
  controller, screen, audio all work outside QEMU). I know this is on the
  list eventually — boot media, PS/2-vs-USB-only keyboards, DS4 over
  UHCI-only ports, VBE mode fallback — but I'm parking it for later. The
  big prerequisites (own bootloader, 64-bit, AHCI, on-disk logging) are
  done now, so this is mostly a matter of picking a victim PC.
