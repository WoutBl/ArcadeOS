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
   launcher have sound effects. (Intel HDA backend still to come.)

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

8. **Networking + REST API** (July 2026). RTL8139 NIC driver
   (`src/rtl8139.c`, fully polled from the idle task) + a minimal
   server-side stack (`src/net.c`): ARP replies, IPv4, ICMP echo, and a
   single-connection TCP server carrying an HTTP REST API on port 80.
   Endpoints: `/api/status` (uptime/tasks/memory), `/api/games`,
   `/api/scores` (parsed .SAV files), `/api/log` (kernel log ring).
   QEMU forwards host port 8080 → `curl localhost:8080/api/scores`
   returns live JSON from the console. This also completes the
   remote-logging half of the logging item. `/api/status` additionally
   reports the running game's LIVE score (SYS_SCORE, auto-reported by
   the SDK via `a.score`), and every HTTP request is logged to
   serial + the kernel log ring.

9. **Launcher polish** (July 2026). Pretty display titles (8.3 filenames
   mapped back — STARCATC.ELF shows as STARCATCH), file sizes, game
   count, and a persistent LAST PLAYED badge: the launcher remembers the
   last game in its own save slot and re-selects it on boot.

10. **Graphics: page flipping** (July 2026). On QEMU's std VGA the
    Bochs display interface gives us a double-height virtual surface;
    presents now render into the hidden page and flip via the Y-offset
    register — tear-free, and the boot console/panic text stays visible
    because the terminal always draws to the displayed page. Falls back
    to the old single-buffer blit on plain VBE hardware. (Mode
    negotiation beyond 640x480 is still future work.)

11. **New game: BLASTER** (July 2026). Robotron-style arena shooter on
    the SDK — move with arrows/left stick, fire in four directions with
    the face buttons (right stick on a DS4), waves of homing enemies,
    persistent high score. Sixth title on the volume.

12. **Graphics: mode negotiation** (July 2026). Stage 2 walks the VBE
    mode list with a preference order (640x480 → 800x600 → 1024x768 →
    any 32bpp LFB mode) instead of hard-requiring 640x480, so real
    hardware whose VBE tables lack 640x480 still gets a framebuffer.
    Kernel blits were already pitch-aware; the SDK cap is now 1024x768
    and games draw at the real a->w/a->h (verified: Pong fullscreen at
    800x600). Fallout fix: the PMM reserves the [4 MiB, 8 MiB) user-
    overlay window so driver DMA structures can never be shadowed by a
    process's page overlays.

13. **Audio: 4-voice mixer + PCM samples** (July 2026). The AC97 ring
    is now fed by a kernel software mixer (`src/audio.c`): 4 voices of
    square tones and uploaded 16-bit mono PCM clips (any rate,
    resampled to 48 kHz), rendered just ahead of the hardware position.
    New `SYS_SOUND_EX` + SDK helpers (`sfx_tone_v`, `sfx_pcm`,
    `sfx_explosion`); overlapping effects now blend instead of cutting
    each other off. BLASTER kills play a generated PCM noise burst, and
    the console plays a PCM boot chime (which doubles as a mixer
    self-test in every boot's audio capture).

14. **xHCI driver** (July 2026). The stub is now a working polled
    driver (`src/xhci.c`): command + event rings, slot enable/address/
    configure, synchronous EP0 control transfers, and a re-armed
    interrupt IN pipe feeding usb_hid_input() — same architecture as
    the UHCI engine, but this is what real post-2015 PCs expose.
    USB boot-protocol keyboards are decoded into set-1 scancodes and
    injected into the SAME input pipeline as PS/2, so both virtual pads
    work over USB. `make run-xhci` boots QEMU with qemu-xhci + usb-kbd;
    verified end to end: Enter on the USB keyboard launches a game.
    This clears the biggest real-hardware bring-up blocker.

15. **Netplay foundation: UDP sockets** (July 2026). The stack grows a
    UDP layer (`src/net.c`): a learned ARP cache with background
    resolution, one datagram socket for the running game behind
    `SYS_NET` (bind/send/recv, 512-byte cap), SDK wrappers
    (`arcade_net_bind/send/recv`, `ARCADE_IP`), and a built-in RFC 862
    echo service on port 7 that the smoke test exercises from the host
    on every CI run. This is the client-side path the REST-only TCP
    server never had — the remaining work for real networked
    multiplayer is game-side (lockstep input exchange in Pong).

16. **Kernel-level universal rewind, v1** (July 2026). Every Ring-3
    game gets save-states for free, with zero cooperation from its
    code: the kernel snapshots the process's writable pages (W^X means
    text never changes) into a 6-slot ring every ~2 s, taken at
    SYS_GFX_PRESENT boundaries — between frames the game's entire
    state IS its memory, so no CPU context needs saving and restore is
    just "return from the same syscall into your past". SELECT+L1
    (Tab+Q) pops one snapshot per press; the chord is filtered out of
    SYS_PAD_READ (with a grace window for the two keys arriving in
    separate polls) so games never see a stray SELECT-quit. Costs
    24 MiB of the 128. Verified in Pong: ball/paddles visibly jump
    back, game keeps running. v2 ideas: input-log replay between
    snapshots for frame-exact scrubbing, hold-to-rewind.

18. **User profiles, scoreboard + player-select** (July 2026). The
    launcher owns profiles now: USERS0.SAV, on-screen-keyboard name
    entry, a two-stage P1/P2 picker pushing the session to the kernel
    (restored on boot, shown to games and /api/status); X opens the
    central HIGH SCORES screen (kernel-tracked HISCORE0.SAV); and the
    SDK grew arcade_choose_players(), the standard 1P/2P(/net) opener
    Pong now uses instead of its hidden Y toggle.

19. **Networked multiplayer** (July 2026). Consoles derive their IP
    from the NIC MAC (10.0.2.<mac[5]>), the stack sends/accepts UDP
    broadcasts, and the SDK provides LAN discovery + handshake
    (arcade_net_host_wait / arcade_net_join_lan — FIND/HERE/JOIN/ACPT
    on the game port). Pong is fully host-authoritative online: the
    joiner streams its pad, the host simulates and streams state, and
    either side leaving pops a farewell card on the other. Verified
    with two QEMU instances on a socket-netdev wire (10.0.2.10 vs
    .11): discovery, paddle control, scoring, and quit propagation.

20. **Kernel system menu — universal pause** (July 2026). SELECT+START
    (Tab+Enter) in any game freezes it inside its own SYS_GFX_PRESENT
    and opens a kernel-drawn overlay (src/sysmenu.c): CONTINUE, one
    line per rewind snapshot with its age (picking one restores it via
    the rewind engine), and QUIT TO LAUNCHER (SYS_EXIT semantics
    without the game's cooperation). The launcher itself is exempt.
    Verified in Pong: menu over the frozen game, restore stepped the
    score back, quit returned to the home screen.

## Up next

21. **Rewind v2 — frame-exact scrubbing** (July 2026). The kernel now
    RECORDS what every game observes per presented frame (both pad
    states + the tick value) and reaches ANY frame by restoring the
    nearest snapshot and letting the game re-execute forward as its own
    replay engine — msleep skipped, blits suppressed, sound/saves/score
    muted during replay. HOLD SELECT+L1 (Tab+Q) and time scrubs
    backward in 8-frame hops with an on-screen indicator and per-hop
    blips; release to resume playing from the frame on screen. A
    virtual-clock offset keeps SYS_TICKS continuous across the cut so
    games that stash absolute tick values never see time jump. The
    lone-SELECT quit-grace is release-based now (a chord may form
    arbitrarily slowly without leaking a quit into the game).

22. **PC-side dev tool + sprite editor** (July 2026). `tools/arcade-dev/
    arcade`: scaffold a game from the SDK template (`arcade new`),
    cross-compile it (`arcade build`), bake it into the volume next to
    the built-ins (`arcade deploy`), and boot it (`arcade run`) — the
    console lists it like any other title. `arcade sprites` opens a
    self-contained browser pixel-art editor (palette/fill/eyedropper/
    undo, 8-32 px, localStorage autosave) that exports SDK `sprite_t`
    C code with SURF_TRANSPARENT for erased pixels. Verified
    end-to-end: scaffolded DEMO, deployed, launched from the launcher,
    scored points, hero sprite on screen.

23. **DHCP client** (July 2026). Standard DORA over broadcast at boot
    (src 0.0.0.0, driven from net_poll): the console takes a proper
    lease when a server exists ([NET] DHCP lease: 10.0.2.15 under
    QEMU slirp, REST + UDP echo verified on the leased address) and
    falls back to the MAC-derived static IP after 5 s when none does
    (verified on a serverless socket-netdev wire — the two-console
    netplay case keeps working unchanged). Real LANs no longer need
    to look like slirp.

## Later

- **Intel HDA audio backend.** AC97 covers QEMU; real hardware
  increasingly exposes HDA only.

- **Visual level editor.** The dev tool covers code + sprites; a
  drag-and-drop level/tilemap editor exporting C data would complete
  the "build a game without leaving the tools" story.



## Not right now

- **Real hardware bring-up** (flashing an actual PC, checking keyboard,
  controller, screen, audio all work outside QEMU). I know this is on the
  list eventually — boot media, PS/2-vs-USB-only keyboards, DS4 over
  UHCI-only ports, VBE mode fallback — but I'm parking it for later. The
  big prerequisites (own bootloader, 64-bit, AHCI, on-disk logging) are
  done now, so this is mostly a matter of picking a victim PC.
