# ArcadeOS

This project is completely created with fable5

A bare-metal x86-64 gaming console operating system, architected like a
modern console: a custom two-stage BIOS bootloader takes the machine from
real mode all the way into 64-bit long mode and boots the kernel straight
into a graphical game launcher, games are ELF64 binaries loaded from a FAT32
game volume and executed in Ring 3, and all hardware access (graphics,
controllers) goes through syscalls. No GRUB — the whole boot chain is ours.

Built on the previously project (name: OS2.0) base kernel: GDT/IDT, bitmap PMM, identity-mapped
paging with per-process page directories, preemptive round-robin scheduler,
TSS-based Ring 3 support, int 0x80 syscalls, VFS + devfs, and ATA PIO.

## Architecture

```
 Ring 3                        Ring 0
┌─────────────────┐   int 0x80  ┌───────────────────────────────────┐
│ launcher.elf     │◄──────────►│ syscall.c  (gfx/pad/fs/timing)    │
│ pong.elf         │            │                                   │
│ (libc/console.h) │            │ console_gfx.c ── fb.c (LFB)       │
└─────────────────┘            │ gamepad.c ── keyboard.c (IRQ1)    │
                               │           └─ usb.c/uhci.c/xhci.c  │
                               │ vfs.c ─┬─ fat32.c ── ata.c        │
                               │        ├─ devfs.c                 │
                               │        └─ fs.c (RAM fs)           │
                               │ scheduler.c / task.c / loader.c   │
                               │ paging.c / pmm.c / heap.c         │
                               └───────────────────────────────────┘
```

### Subsystems

| Subsystem | Files | Notes |
|---|---|---|
| Bootloader | `boot/stage1.asm`, `boot/stage2.asm` | Two-stage BIOS bootloader living in the FAT32 reserved sectors: stage 1 is the volume boot record around the BPB; stage 2 enables A20, collects the E820 memory map, loads the kernel flat binary to 1 MiB (INT 13h chunks + protected-mode copies), sets a 640x480x32 VBE linear-framebuffer mode, builds identity page tables (first 4 GiB, 2 MiB pages), and enters **long mode** with a multiboot-compatible boot info struct |
| Framebuffer | `fb.c`, `boot.asm` | 640x480x32 LFB set up by the bootloader via VBE; mapped into the identity map by `paging.c` |
| Graphics API | `console_gfx.c/h`, `fb.c` | Double-buffered software renderer: clear, rects, lines, sprites (alpha-keyed), 8x8 font text; presents are **tear-free page flips** via the Bochs display interface (double-height VRAM + Y-offset) with a plain-VBE blit fallback |
| Boot console | `vga.c` | The classic terminal API now renders glyphs onto the framebuffer (VGA text fallback kept); all output mirrored to COM1 (`serial.c`) |
| Controllers | `gamepad.c` | Merges sources into 4 logical pads; the keyboard is split into TWO virtual pads for local 2-player (pad 0 = arrows/XZCV, pad 1 = WASD + R/T/F/G); USB pads merge into pad 0; press-edge latching so slow frame loops never miss a tap |
| USB host | `pci.c`, `usb.c`, `uhci.c`, `xhci.c` | Full UHCI transfer engine: frame list + QH/TD schedule, synchronous control transfers (enumeration: GET_DESCRIPTOR / SET_ADDRESS / SET_CONFIGURATION), polled interrupt IN pipes for HID reports; DualShock 4 reports decoded in `usb.c` and merged into pad 0; xHCI remains an architectural stub |
| Storage | `ahci.c`, `ata.c`, `disk.c`, `fat32.c` | AHCI (SATA) driver preferred — what real hardware exposes — with legacy ATA PIO as fallback, dispatched through `disk.c`; FAT32 (8.3 names) mounted at `/games`; write path (cluster alloc, FAT mirroring, directory updates) backs the save-data API |
| Save data | `fat32.c`, `syscall.c` | "Memory card" semantics: whole-file `SYS_SAVE`/`SYS_LOAD` on the game volume; games persist high scores across power cycles |
| Kernel log | `klog.c`, `serial.c` | Everything mirrored to COM1 also lands in a 32 KiB ring buffer, flushed to `KERNEL.LOG` on the game volume by the idle task (~2 s cadence) — boot logs survive on disk even without a serial cable |
| Networking | `rtl8139.c`, `net.c` | RTL8139 NIC (polled from the idle task) + minimal server stack: ARP, IPv4, ICMP echo, single-connection TCP, and an **HTTP REST API** on port 80 — `/api/status` (incl. the live score of the running game), `/api/games`, `/api/scores`, `/api/log` (which also records every HTTP request). With QEMU: `curl http://localhost:8080/api/status` |
| Audio | `audio.c`, `ac97.c`, `pcspk.c` | `SYS_SOUND` square-wave tones (freq + duration): synthesized 48 kHz stereo PCM through the AC97 codec's DMA engine when present, PC speaker (PIT channel 2) fallback otherwise; launcher and all games have sound effects |
| Game loading | `loader.c` | ELF loaded via VFS into a fresh page directory, 16 KiB user stack, Ring 3 entry via iret |
| Crash safety | `paging.c` | A Ring 3 page fault kills the game and returns to the launcher instead of panicking the console |

### Syscalls (int 0x80)

This is the COMPLETE kernel ABI — every number not listed returns -1.
All pointers are validated against the calling process's page tables
(`paging_user_access_ok`); a pointer outside the process's own mapped,
user-accessible memory makes the syscall fail instead of touching
kernel memory.

Process & I/O (inherited from the OS2.0 base):

| # | Name | Purpose |
|---|---|---|
| 0 | `SYS_EXIT` | Terminate the calling task, wake its waiters |
| 1 | `SYS_WRITE` | Write to an fd (routed through the VFS; 0/1/2 → `/dev/tty`) |
| 2 | `SYS_YIELD` | Give up the CPU voluntarily |
| 3 | `SYS_READ` | Read from an fd (stdin falls back to the keyboard) |
| 4 | `SYS_SPAWN` | Launch an ELF (the launcher's game-start path); child inherits fds 0–2 |
| 5 | `SYS_WAIT` | Block until a PID exits |
| 16 | `SYS_OPEN` | Open a VFS path into the fd table |
| 17 | `SYS_CLOSE` | Close an fd |

Numbers 6–15 and 18–20 are retired: they were the OS2.0 shell ABI
(RAM-fs file ops, pipes, dup2, signals) that no ArcadeOS app ever
called — undocumented attack surface, now stripped from the kernel.
The numbers stay reserved so old binaries get a clean -1.

Console subsystem:

| # | Name | Purpose |
|---|---|---|
| 21 | `SYS_GFX_INFO` | Framebuffer geometry (`gfx_info_t`) |
| 22 | `SYS_GFX_PRESENT` | Blit a full-screen user pixel buffer |
| 23 | `SYS_PAD_READ` | Controller state (`pad_state_t`), polls USB |
| 24 | `SYS_TICKS` | Milliseconds since boot |
| 25 | `SYS_MSLEEP` | Sleep (PIT-driven, CPU halts) |
| 26 | `SYS_READDIR` | Directory listing (used by the launcher) |
| 27 | `SYS_SAVE` | Whole-file save to the game volume (max 64 KiB) |
| 28 | `SYS_LOAD` | Whole-file load; returns bytes read |
| 29 | `SYS_SOUND` | Play a square-wave tone (EBX = Hz, ECX = ms; 0 Hz stops) |
| 30 | `SYS_SCORE` | Report the live score (served by `/api/status` while playing) |
| 31 | `SYS_SOUND_EX` | Mixer voices (`sound_req_t`): square tones + 16-bit mono PCM clips |
| 32 | `SYS_NET` | UDP netplay socket (`net_req_t`): bind/send/recv, 512-byte datagrams |

User-space API: `libc/console.h` (syscall wrappers + software drawing helpers).
Shared ABI structs: `include/console_abi.h`.

### Game SDK

All games are written against **libarcade** ([sdk/arcade.h](sdk/arcade.h),
docs in [sdk/README.md](sdk/README.md)): a fixed-timestep game loop, input
edge detection, fixed-point math, sprites, entities with AABB collision,
save slots (slot 0-9 per game), canned sound effects, and a PRNG.
`apps/starcatch.c` (STARCATCH) is the reference game — every SDK feature
in under 200 lines.

## Building

Prerequisites (macOS): `brew install nasm x86_64-elf-gcc qemu`

```sh
make            # bootloader + kernel + arcadeos.img (bootable FAT32 volume)
make run        # boot in QEMU with a window (disk attached via AHCI)
make run-headless   # headless: serial.log + qemu-monitor.sock
make run-ide        # headless, disk on legacy IDE (tests the ATA PIO fallback)
```

`tools/mkfat32.py` builds a single self-booting image: the two bootloader
stages and the kernel flat binary go into the FAT32 reserved sectors, and
`LAUNCHER.ELF` plus the game ELFs are copied into the root directory. Add a
new game by dropping `apps/<name>.c` in, adding `$(BUILD)/<name>.elf` to
`APPS` in the Makefile — the launcher lists every `*.ELF` on the volume
automatically.

### Boot flow

1. BIOS loads sector 0 (stage 1, embedded around the FAT32 BPB) at `0x7C00`
2. Stage 1 reads stage 2 from reserved sectors (LBA 8) via INT 13h LBA
3. Stage 2: A20 → E820 memory map → kernel to `0x100000` (32 KiB chunks,
   copied above 1 MiB through brief protected-mode hops) → VBE 640x480x32
   LFB mode-set → identity page tables (first 4 GiB, 2 MiB pages) →
   PAE + EFER.LME + paging = **64-bit long mode** → jump to the kernel with
   `RDI=0xA5CADE05`, `RSI=boot info` (SysV args)
4. The kernel entry stub (`boot.asm`) zeroes `.bss` and calls `kernel_main`

The kernel and games are 64-bit throughout: 4-level paging (2 MiB kernel
identity pages, 4 KiB user pages overlaid per process), a 64-bit GDT/IDT/TSS,
and ELF64 user binaries entered at `main(argc, argv)` per the SysV ABI.
Kernel and games are compiled without SSE/MMX (`-mno-sse -msoft-float`) —
context switches don't save FPU state.

Note: rebuilding regenerates `arcadeos.img`, which wipes save files along with
it. Keep a copy of the image if you care about your high scores.

## Controls (keyboard = two virtual pads)

The keyboard is split into two logical pads for local 2-player; a USB
controller (DualShock 4) always merges into pad 0 (player 1).

| Input | Player 1 (pad 0) | Player 2 (pad 1) |
|---|---|---|
| D-pad | Arrow keys | W / A / S / D |
| A / B / X / Y | X / Z / C / V | R / T / F / G |
| START / SELECT | Enter / Tab | — |
| L1 / R1 | Q / E | — |

Launcher: up/down select, A or START to play. Pong: SELECT or B quits back
to the launcher, START pauses, and **Y (V key) toggles 1P-vs-CPU / 2P mode**
(player 2 moves the right paddle with W/S).

## Real controller (DualShock 4 over micro-USB)

The UHCI driver enumerates a passed-through DualShock 4 (vendor 0x054C,
product 0x05C4/0x09CC) and maps it to pad 0 alongside the keyboard:
Cross/Circle/Square/Triangle → A/B/X/Y, Options/Share → START/SELECT,
D-pad hat + both analog sticks + trigger axes.

```sh
sudo make run-ds4    # root required, see below
```

On macOS the Apple HID kernel driver owns the controller's USB interface;
an unprivileged QEMU can attach the device (it enumerates fine) but every
interrupt transfer fails with a timeout. Running QEMU as root lets libusb
seize the interface (`USBInterfaceOpenSeize`). While the VM runs, macOS
does not see the controller; unplug/replug hands it back. Adjust the
product id in the Makefile for a v1 pad (0x05C4).

## Games

All games (and the launcher) are built on the SDK:

- `apps/launcher.c` – the home screen (Ring 3 init process)
- `apps/pong.c` – reference title: pad input + fixed-point physics + present
- `apps/snake.c` – grid movement, screen wrapping, persistent high score
- `apps/breakout.c` – brick grid sized from `gfx_info`, persistent high score
- `apps/starcatch.c` – STARCATCH, the SDK reference game (sprites, entities,
  collision, save slots — all through `sdk/arcade.h`); ships as
  `STARCATC.ELF` (8.3 filename limit; the launcher shows the full title)
- `apps/blaster.c` – BLASTER, Robotron-style arena shooter: move with one
  cluster, fire in four directions with the other (right stick on a DS4)

Games persist data with `save_data("NAME.SAV", &blob, len)` /
`load_data(...)` from `libc/console.h` — whole-file reads/writes of 8.3-named
files on the game volume, surviving reboots.

## License

MIT — see [LICENSE](LICENSE). The embedded 8x8 font is derived from the
public-domain `font8x8` glyph set.
