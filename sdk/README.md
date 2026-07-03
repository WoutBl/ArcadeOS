# ArcadeOS Game SDK (libarcade)

The framework for writing ArcadeOS games. It layers on top of the syscall
libc and provides what every game needs so you don't hand-roll it again:

| Feature | API |
|---|---|
| Fixed-timestep game loop (~60 FPS) | `arcade_init()`, `arcade_frame()` |
| Input with edge detection | `a.pressed`, `a.released`, `a.held`, `a.pad` (analog sticks) |
| Fixed-point math (24.8) | `fx_t`, `FX()`, `FX_INT()`, `FX_MUL()` |
| Sprites (color-keyed, scaled) | `sprite_t`, `arcade_draw_sprite()` |
| Entities (position/velocity/collision box) | `entity_t`, `arcade_entity_move/bounce/overlap/draw()` |
| AABB collision | `arcade_aabb()` |
| Save slots (0-9 per game) | `arcade_save()`, `arcade_load()` |
| Canned sound effects | `sfx_move/select/hit/score/lose/gameover()` |
| PRNG | `arcade_srand()`, `arcade_rand()`, `arcade_rand_range()` |

Everything from `libc/console.h` is available too (drawing helpers,
`sound()`, raw syscalls). The reference game is
[apps/starcatch.c](../apps/starcatch.c) (STARCATCH) — it uses every SDK
feature in under 200 lines. Pong, Snake, Breakout, and the launcher are
all built on the SDK too.

## Starting a new game

1. Create `apps/mygame.c`:

```c
#include "../sdk/arcade.h"
#include "../libc/syscall.h"

int main(void) {
    arcade_t a;
    if (arcade_init(&a) != 0) return 1;

    entity_t ball = { .x = FX(100), .y = FX(100),
                      .vx = FX(3),  .vy = FX(2),
                      .w = 16, .h = 16, .active = 1 };

    while (arcade_frame(&a)) {
        if (a.pressed & PAD_BTN_SELECT) exit(0);   /* Back to launcher */

        if (arcade_entity_bounce(&ball, a.w, a.h)) sfx_hit();

        surf_clear(&a.screen, rgb(8, 10, 30));
        surf_fill_rect(&a.screen, FX_INT(ball.x), FX_INT(ball.y),
                       ball.w, ball.h, rgb(255, 255, 255));
    }
    return 0;
}
```

2. Add `$(BUILD)/mygame.elf` to `APPS` in the Makefile.
3. `make run` — the launcher lists every `*.ELF` on the volume
   automatically.

## Rules of the road

- **No heap**: games have no malloc. Use static arrays (the SDK's
  framebuffer is static too).
- **No floats**: games build with `-mno-sse -msoft-float` (the kernel
  doesn't save FPU state). Use the fixed-point helpers.
- **8.3 names**: keep the game name ≤ 7 characters for save slots
  (`arcade_save("MYGAME", 0, ...)` → `MYGAME0.SAV`).
- Rebuilding regenerates the game volume, which wipes save files.
