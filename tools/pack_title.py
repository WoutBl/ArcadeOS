#!/usr/bin/env python3
"""
ArcadeOS – append a display-title trailer to a built game ELF.

Usage: pack_title.py <elf_path> <source.c>

Scans the game's C source for an ARCADE_GAME("Title") declaration
(sdk/arcade.h) and appends a 32-byte trailer to the ELF: a 4-byte magic
plus the title, NUL-padded (see include/gamemeta.h — keep both in sync).
Falls back to the ELF's own base filename, uppercased, when the source
doesn't declare one, so packaging never fails a build.

Idempotent: re-running on an already-packed file replaces the trailer
instead of stacking another one, so `make` can call this every build.
"""
import re
import sys
import os

MAGIC = b"ARCM"
TITLE_LEN = 28
TRAILER_SIZE = len(MAGIC) + TITLE_LEN


def extract_title(source_path, fallback):
    try:
        with open(source_path, "r") as f:
            text = f.read()
    except OSError:
        return fallback
    m = re.search(r'ARCADE_GAME\(\s*"([^"]+)"\s*\)', text)
    return m.group(1) if m else fallback


def main():
    if len(sys.argv) != 3:
        print("usage: pack_title.py <elf_path> <source.c>", file=sys.stderr)
        return 1
    elf_path, source_path = sys.argv[1], sys.argv[2]

    fallback = os.path.splitext(os.path.basename(elf_path))[0].upper()
    title = extract_title(source_path, fallback).upper()
    title_bytes = title.encode("ascii", "replace")[:TITLE_LEN - 1]
    title_bytes = title_bytes.ljust(TITLE_LEN, b"\0")

    with open(elf_path, "rb") as f:
        data = f.read()

    # Idempotent: strip a previously-appended trailer before adding the
    # current one, so repeated builds don't grow the file.
    if len(data) >= TRAILER_SIZE and data[-TRAILER_SIZE:-TRAILER_SIZE + 4] == MAGIC:
        data = data[:-TRAILER_SIZE]

    with open(elf_path, "wb") as f:
        f.write(data)
        f.write(MAGIC)
        f.write(title_bytes)

    print(f"pack_title: {elf_path} -> \"{title}\"")
    return 0


if __name__ == "__main__":
    sys.exit(main())
