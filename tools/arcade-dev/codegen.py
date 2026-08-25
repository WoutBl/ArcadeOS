#!/usr/bin/env python3
"""
ArcadeOS – no-code game builder: JSON game definition -> generated C.

The generated file is almost pure data (struct literals against
sdk/arcade_builder.h's types) plus a 3-line main() — the actual game
logic lives once, hand-written, in sdk/arcade_builder.c. See that file
and the plan this was built from for the full picture.

Usage as a library: `generate(game_dict) -> str`. Also runnable
standalone for local testing: `codegen.py in.json out.c`.
"""
import json
import sys

MAX_ENTITIES = 32
MAX_BG_LAYERS = 3
MAX_TILE_LAYERS = 3
MAX_LEVELS = 16

BEHAVIORS = {"static": "BEH_STATIC", "playerMove": "BEH_PLAYER_MOVE",
             "bounce": "BEH_BOUNCE", "home": "BEH_HOME", "patrol": "BEH_PATROL"}
ROLES = {"player": "ROLE_PLAYER", "enemy": "ROLE_ENEMY", "pickup": "ROLE_PICKUP"}
EFFECTS = {"none": "FX_NONE", "score": "FX_SCORE", "loseLife": "FX_LOSE_LIFE",
           "win": "FX_WIN", "gameover": "FX_GAMEOVER"}
SESSIONS = {"1p": "BUILDER_SESSION_1P", "2p": "BUILDER_SESSION_2P",
            "choose": "BUILDER_SESSION_CHOOSE"}


class BuildError(ValueError):
    """A problem with the game definition itself (not a codegen bug) —
    the message is meant to be shown directly in the builder UI."""


def _c_string(s):
    return '"' + str(s).replace("\\", "\\\\").replace('"', '\\"') + '"'


def _hex_to_0xrrggbb(h):
    h = h.lstrip("#")
    if len(h) != 6:
        raise BuildError(f"bad color {h!r} — want \"#RRGGBB\"")
    return "0x" + h.upper()


def game_tag(name):
    """7-char save-slot-safe tag (sdk/arcade.h caps save names at 7)."""
    tag = "".join(c for c in str(name).upper() if c.isalnum())[:7]
    if not tag:
        raise BuildError("name must contain at least one letter or digit")
    return tag


def _gen_sprite(ident, spr):
    w, h = int(spr["w"]), int(spr["h"])
    pixels = spr["pixels"]
    if len(pixels) != w * h:
        raise BuildError(f"sprite {ident}: expected {w*h} pixels, got {len(pixels)}")
    vals = ["SURF_TRANSPARENT" if p is None else _hex_to_0xrrggbb(p) for p in pixels]
    px = f"static const uint32_t {ident}_px[] = {{\n    " + ", ".join(vals) + "\n};\n"
    spr_c = f"static const sprite_t {ident}_spr = {{ {ident}_px, {w}, {h} }};\n"
    return px + spr_c


def _gen_tile_layer(ident, layer, is_last):
    w, h, tile = int(layer["w"]), int(layer["h"]), int(layer["tile"])
    cells = layer["cells"]
    if len(cells) != w * h:
        raise BuildError(f"tile layer {ident}: expected {w*h} cells, got {len(cells)}")
    colors = layer["colors"]
    cells_c = (f"static const uint8_t {ident}_cells[] = {{ " +
               ", ".join(str(int(c)) for c in cells) + " };\n")
    colors_c = (f"static const uint32_t {ident}_colors[] = {{ " +
                ", ".join(_hex_to_0xrrggbb(c) for c in colors) + " };\n")
    solid = int(layer.get("solidMask", 0)) if is_last else 0
    entry = (f"{{ {{ {ident}_cells, {w}, {h}, {tile} }}, "
             f"{ident}_colors, {solid}u }}")
    return cells_c + colors_c, entry


def _gen_level(idx, level, out):
    """Emits one level's sprite/tile/bg/entity arrays (prefixed l{idx}_
    to keep symbols unique across levels) into `out`. Returns the
    builder_level_t entry text for this level."""
    tag = f"level {idx + 1}"
    entities = level.get("entities", [])
    if len(entities) == 0:
        raise BuildError(f"{tag}: needs at least one entity (usually the player)")
    if len(entities) > MAX_ENTITIES:
        raise BuildError(f"{tag}: too many entities ({len(entities)} > {MAX_ENTITIES})")
    if not any(e.get("role") == "player" for e in entities):
        raise BuildError(f"{tag}: no entity has role \"player\" — add one")

    bg_layers = level.get("bgLayers", [])
    tile_layers = level.get("tileLayers", [])
    if len(bg_layers) > MAX_BG_LAYERS:
        raise BuildError(f"{tag}: too many background layers ({len(bg_layers)} > {MAX_BG_LAYERS})")
    if len(tile_layers) > MAX_TILE_LAYERS:
        raise BuildError(f"{tag}: too many tilemap layers ({len(tile_layers)} > {MAX_TILE_LAYERS})")

    pfx = f"l{idx}_"

    for i, e in enumerate(entities):
        out.append(_gen_sprite(f"{pfx}ent{i}", e["sprite"]))

    tile_entries = []
    for i, layer in enumerate(tile_layers):
        decl, entry = _gen_tile_layer(f"{pfx}tile{i}", layer, i == len(tile_layers) - 1)
        out.append(decl)
        tile_entries.append(entry)
    if tile_entries:
        out.append(f"static const builder_tile_layer_t {pfx}tile_layers[] = {{\n    " +
                   ",\n    ".join(tile_entries) + "\n};")

    bg_entries = []
    for bg in bg_layers:
        bg_entries.append(
            f"{{ {_hex_to_0xrrggbb(bg['color'])}, {int(bg.get('scrollSpeed', 1))}, "
            f"{int(bg.get('bandY', 0))}, {int(bg.get('bandH', 60))} }}")
    if bg_entries:
        out.append(f"static const builder_bg_layer_t {pfx}bg_layers[] = {{\n    " +
                   ",\n    ".join(bg_entries) + "\n};")

    ent_entries = []
    for i, e in enumerate(entities):
        role = ROLES.get(e.get("role"))
        if role is None:
            raise BuildError(f"{tag}, entity {i}: bad role {e.get('role')!r}")
        behavior = BEHAVIORS.get(e.get("behavior", "static"))
        if behavior is None:
            raise BuildError(f"{tag}, entity {i}: bad behavior {e.get('behavior')!r}")
        hit = e.get("onPlayerHit", {"effect": "none"})
        effect = EFFECTS.get(hit.get("effect", "none"))
        if effect is None:
            raise BuildError(f"{tag}, entity {i}: bad onPlayerHit.effect {hit.get('effect')!r}")
        rule = (f"{{ {effect}, {int(hit.get('amount', 0))}, "
               f"{1 if hit.get('removeSelf') else 0} }}")
        ent_entries.append(
            f"{{ {role}, {int(e.get('playerIndex', 0))}, &{pfx}ent{i}_spr, "
            f"{int(e['x'])}, {int(e['y'])}, {int(e.get('w', 0))}, {int(e.get('h', 0))}, "
            f"{int(e.get('vx', 0))}, {int(e.get('vy', 0))}, {int(e.get('speed', 2))}, "
            f"{behavior}, {int(e.get('patrolX2', 0))}, {int(e.get('patrolY2', 0))}, "
            f"{rule} }}")
    out.append(f"static const builder_entity_t {pfx}entities[] = {{\n    " +
               ",\n    ".join(ent_entries) + "\n};")

    return (f"{{ {pfx + 'bg_layers' if bg_entries else '0'}, {len(bg_entries)}, "
           f"{pfx + 'tile_layers' if tile_entries else '0'}, {len(tile_entries)}, "
           f"{pfx}entities, {len(ent_entries)} }}")


def generate(game, source_note=""):
    """game: parsed JSON dict. Returns generated C source as a string.
    Raises BuildError for anything wrong with the definition itself."""
    name = game_tag(game.get("name", ""))
    title = game.get("title") or name
    session = SESSIONS.get(game.get("session", "1p"))
    if session is None:
        raise BuildError(f"bad session {game.get('session')!r}")

    levels = game.get("levels")
    if not levels:
        # Back-compat with the pre-levels flat single-world schema.
        levels = [{"bgLayers": game.get("bgLayers", []),
                  "tileLayers": game.get("tileLayers", []),
                  "entities": game.get("entities", [])}]
    if len(levels) > MAX_LEVELS:
        raise BuildError(f"too many levels ({len(levels)} > {MAX_LEVELS})")

    out = []
    out.append("/* Generated by tools/arcade-dev/codegen.py — do not hand-edit. */")
    if source_note:
        out.append(f"/* {source_note} */")
    out.append('#include "../sdk/arcade_builder.h"')
    out.append("")
    out.append(f"ARCADE_GAME({_c_string(title)});")
    out.append("")

    level_entries = [_gen_level(i, lvl, out) for i, lvl in enumerate(levels)]

    out.append("static const builder_level_t levels[] = {\n    " +
               ",\n    ".join(level_entries) + "\n};")

    out.append("static const builder_game_t cfg = {")
    out.append(f"    {_c_string(name)}, {_c_string(title)}, {session},")
    out.append(f"    {int(game.get('lives', 3))}, {int(game.get('winScore', 0))}, "
               f"{int(game.get('timeLimitSec', 0))},")
    out.append(f"    levels, {len(level_entries)}")
    out.append("};")
    out.append("")
    out.append("int main(void) { arcade_builder_run(&cfg); return 0; }")
    return "\n".join(out) + "\n"


def main():
    if len(sys.argv) != 3:
        print("usage: codegen.py <in.json> <out.c>", file=sys.stderr)
        return 1
    with open(sys.argv[1]) as f:
        game = json.load(f)
    try:
        code = generate(game, source_note=f"from {sys.argv[1]}")
    except BuildError as e:
        print(f"codegen: {e}", file=sys.stderr)
        return 1
    with open(sys.argv[2], "w") as f:
        f.write(code)
    print(f"codegen: wrote {sys.argv[2]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
