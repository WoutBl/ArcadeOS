# arcade — the ArcadeOS PC-side dev tool

Build games for the console on a normal PC. The console picks up any
`*.ELF` in the FAT32 volume root, so shipping a game is just deploying
the volume image (or, someday, copying it onto removable media).

## Quick start

```sh
tools/arcade-dev/arcade new mygame     # scaffold a project
tools/arcade-dev/arcade run mygame     # build + deploy + boot QEMU
```

`mygame/` gets a `main.c` (the standard SDK skeleton: fixed-timestep
loop, edge-detected input, save slot, live score) and a `sprites.h`
with a starter hero sprite. Edit, `arcade run`, repeat.

## Commands

| command | what it does |
|---|---|
| `arcade new NAME [DIR]` | scaffold a game project from the SDK template |
| `arcade build [DIR]` | compile `DIR` → `NAME.ELF` (cross toolchain + libarcade) |
| `arcade deploy [DIR]` | build, then bake the game into `arcadeos.img` next to the built-ins |
| `arcade run [DIR]` | deploy + boot QEMU with the volume |
| `arcade sprites` | open the pixel-art sprite editor in your browser |
| `arcade levels` | open the tilemap level editor in your browser |

## The sprite editor

`arcade sprites` opens a self-contained editor (no install, no deps):
paint on an 8–32 px grid with palette/fill/eyedropper/undo, then
**EXPORT C** produces an SDK `sprite_t` ready to paste into your
game's `sprites.h`. Erased pixels export as `SURF_TRANSPARENT` and are
skipped by `arcade_draw_sprite()`. Work auto-saves to the browser.

## The level editor

`arcade levels` opens the tilemap editor: paint a grid of tiles
(20x15 @ 32px fills one console screen; 40x30 and 80x60 variants),
16 tile slots with editable colors and per-tile SOLID flags, fill
tool, one-click wall border, right-drag erases. **EXPORT C** produces
a `tilemap_t` + color table + solidity bitmask for the SDK:

```c
arcade_draw_tilemap(&a.screen, &level1_map, level1_colors);
if (arcade_tilemap_hits(&map, LEVEL1_SOLID, x, y, w, h)) { /* wall */ }
arcade_tile_at(&map, px, py);   /* what's under this pixel? */
```

The game template ships with a playable example: a walled arena with
solid blocks and collectible pickups (see `level.h` + `main.c`).

## Notes

- Game names come from the project directory: 8.3 uppercase
  (`mygame/` → `MYGAME.ELF`, max 8 chars).
- A plain `make` in the repo regenerates `arcadeos.img` **without**
  deployed games — rerun `arcade deploy` afterwards (same rule as
  save data).
- The SDK API reference lives in `sdk/README.md`.
