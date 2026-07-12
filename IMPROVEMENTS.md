# ArcadeOS – Improvement Notes

Research notes from a code review pass (no code changed). Grounded in the
actual source (`src/syscall.c`, `src/paging.c`, `src/scheduler.c`, `loader.c`)
rather than generic OS advice.

## Security / correctness

- **No W^X anywhere.** Every user page (`loader.c`) is mapped
  `PRESENT | READ_WRITE | USER` with no NX bit, and the kernel never sets
  `EFER.NXE`. Every game's stack and heap are simultaneously writable and
  executable — a single buffer overflow in any game (or a future
  third-party game via the planned PC-side editor) is trivial code
  execution. Cheap, high-value fix: one EFER bit + one PTE bit.
- **Syscalls trust raw user pointers with zero validation.** `SYS_WRITE`,
  `SYS_WRITEFILE`, `SYS_SAVE`, `SYS_GFX_PRESENT`, etc. dereference whatever
  pointer the game passes with no check that it lies inside that process's
  mapped, non-kernel address range (`SYS_GFX_PRESENT` bounds-checks its
  buffer; most others don't check anything). No `copy_from_user` /
  pointer-validation helper exists — worth adding one and routing every
  syscall through it.
- **Save writes aren't atomic.** `fat32_save` writes in place; a power cut
  mid-write (plausible on an arcade cabinet) can corrupt a high-score file.
  Write-to-temp-then-rename, or a checksum + backup slot, would fix this
  cheaply.
- **Panic path doesn't flush the log.** The kernel log only reaches
  `KERNEL.LOG` via the idle task every ~2s; the page-fault/panic handler
  disables interrupts and halts forever without an explicit `klog_flush()`,
  so a real panic on hardware without a serial cable leaves no trace of why.

## Roadmap / compatibility gaps

- **xHCI is a stub.** This is the real blocker for "real hardware
  bring-up" — most PCs built after ~2015 only expose xHCI ports, so
  without it there may be no keyboard or controller at all on real
  hardware, PS/2 aside. Prioritize this over other roadmap items if
  hardware bring-up matters soon.
- Single hardcoded 640x480x32 mode (already flagged as next on the
  roadmap) and audio limited to square-wave tones — no sample/PCM mixing
  pipeline yet.
- **Networked multiplayer isn't on the roadmap at all**, but the pieces
  are already there: local 2-player pad virtualization plus a working
  TCP/IP stack. Currently server-only/single-connection, so this needs a
  client-side socket path, but it's a natural next feature given what's
  already built.

## Engineering hygiene

- **Docs undersell the actual syscall surface.** `syscall.c` implements a
  full shell-style ABI (spawn/wait, pipes, dup2, signals, an in-RAM
  filesystem with cd/mkdir/rm) inherited from the OS2.0 base, none of
  which appears in the README's documented syscall table (which only
  lists the console syscalls 21–30). Either document it or strip the
  unused half — right now it's dead attack surface nobody described.
- **No automated testing at all** — no CI, no scripted boot smoke-test.
  For ~8,000 lines of freestanding C this is thin; a simple "boot headless
  in QEMU, grep serial.log for expected markers" GitHub Action would catch
  regressions for free.

## A genuinely new idea: kernel-level universal rewind

*Caveat: I can't guarantee true novelty — the OS design space is huge and I
can't search every research paper — so treat this as "I don't know of
prior art," not a certainty.*

ArcadeOS's SDK games are already deterministic: fixed timestep, an owned
PRNG, and edge-detected input — the exact ingredients emulators use for
save-states and rewind, but here they exist natively in a non-emulated,
bare-metal OS. That opens an unusual possibility: a **kernel-level
universal rewind**, not implemented per-game but as an OS primitive every
Ring-3 process gets for free.

Mechanically: the scheduler already context-switches per process and the
loader already does copy-on-write-style page overlaying onto the identity
map. Extend that into periodic COW snapshots of a process's user pages
(every N frames) plus a ring buffer of its gamepad-syscall inputs since
the last snapshot. Holding a controller chord (e.g. SELECT+L1) tells the
kernel to restore the nearest snapshot and mechanically replay the logged
inputs back to the target frame — scrubbing time backward and forward for
any game, with no cooperation from the game's code, no save-state API to
implement, and no emulation layer.

Emulators do this for known, emulated hardware; record-replay debuggers
(rr and similar) do it for arbitrary processes but for debugging, not as a
live user-facing feature. The combination — "OS gives every native
process free save-states via COW + input replay" as a live console
feature — fits this project specifically because the SDK already
guarantees the determinism that makes it cheap.
