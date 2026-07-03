#!/usr/bin/env python3
"""
ArcadeOS - FAT32 game volume builder.

Creates a FAT32 "superfloppy" (BPB at LBA 0, no MBR) and copies the given
files into the root directory using 8.3 short names. This is the volume
the ArcadeOS fat32.c driver mounts at /games.

Usage: python3 mkfat32.py <image.img> <size_mb> <file1> [file2 ...]
"""

import struct
import sys
import os

SECTOR = 512
SPC = 8                      # sectors per cluster (4 KiB clusters)
RESERVED = 32                # reserved sectors (BPB, FSInfo, backup boot)
NUM_FATS = 2


def short_name(filename):
    """'pong.elf' -> b'PONG    ELF' (8.3, space padded, uppercased)."""
    base = os.path.basename(filename).upper()
    if "." in base:
        name, ext = base.rsplit(".", 1)
    else:
        name, ext = base, ""
    name = "".join(c for c in name if c.isalnum() or c in "_-")[:8]
    ext = "".join(c for c in ext if c.isalnum())[:3]
    return name.ljust(8).encode("ascii") + ext.ljust(3).encode("ascii")


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(1)

    img_path = sys.argv[1]
    size_mb = int(sys.argv[2])
    files = sys.argv[3:]

    total_sectors = size_mb * 1024 * 1024 // SECTOR

    # Estimate FAT size: data clusters ~= total/SPC, 4 bytes per entry
    clusters_est = total_sectors // SPC
    fat_size = (clusters_est * 4 + SECTOR - 1) // SECTOR + 1

    data_start = RESERVED + NUM_FATS * fat_size
    total_clusters = (total_sectors - data_start) // SPC
    if total_clusters < 65525:
        # Not strictly FAT32-compliant, but our driver only checks the
        # BPB fs_type string. Warn anyway.
        print(f"mkfat32: note: {total_clusters} clusters (<65525); "
              f"fine for ArcadeOS, may confuse other tools")

    # ---- Boot sector / BPB ----
    bpb = bytearray(SECTOR)
    bpb[0:3] = b"\xEB\x58\x90"                      # jmp short
    bpb[3:11] = b"ARCADEOS"                          # OEM name (8 bytes exact)
    struct.pack_into("<H", bpb, 11, SECTOR)          # bytes/sector
    bpb[13] = SPC                                    # sectors/cluster
    struct.pack_into("<H", bpb, 14, RESERVED)        # reserved sectors
    bpb[16] = NUM_FATS                               # FAT count
    struct.pack_into("<H", bpb, 17, 0)               # root entries (FAT32: 0)
    struct.pack_into("<H", bpb, 19, 0)               # total sectors 16 (0)
    bpb[21] = 0xF8                                   # media descriptor
    struct.pack_into("<H", bpb, 22, 0)               # FAT size 16 (0)
    struct.pack_into("<H", bpb, 24, 63)              # sectors/track
    struct.pack_into("<H", bpb, 26, 255)             # heads
    struct.pack_into("<I", bpb, 28, 0)               # hidden sectors
    struct.pack_into("<I", bpb, 32, total_sectors)   # total sectors 32
    struct.pack_into("<I", bpb, 36, fat_size)        # FAT size 32
    struct.pack_into("<H", bpb, 40, 0)               # ext flags
    struct.pack_into("<H", bpb, 42, 0)               # fs version
    struct.pack_into("<I", bpb, 44, 2)               # root cluster
    struct.pack_into("<H", bpb, 48, 1)               # FSInfo sector
    struct.pack_into("<H", bpb, 50, 6)               # backup boot sector
    bpb[64] = 0x80                                   # drive number
    bpb[66] = 0x29                                   # boot signature
    struct.pack_into("<I", bpb, 67, 0xA2CADE05)       # volume id
    bpb[71:82] = b"ARCADEOS   "                       # volume label (11 bytes exact)
    bpb[82:90] = b"FAT32   "                         # fs type
    bpb[510] = 0x55
    bpb[511] = 0xAA

    # ---- FSInfo ----
    fsinfo = bytearray(SECTOR)
    struct.pack_into("<I", fsinfo, 0, 0x41615252)    # lead signature
    struct.pack_into("<I", fsinfo, 484, 0x61417272)  # struct signature
    struct.pack_into("<I", fsinfo, 488, 0xFFFFFFFF)  # free count (unknown)
    struct.pack_into("<I", fsinfo, 492, 0xFFFFFFFF)  # next free (unknown)
    fsinfo[510] = 0x55
    fsinfo[511] = 0xAA

    # ---- FAT + root directory + file data ----
    fat = [0] * (fat_size * SECTOR // 4)
    fat[0] = 0x0FFFFFF8                              # media descriptor entry
    fat[1] = 0x0FFFFFFF                              # EOC
    fat[2] = 0x0FFFFFFF                              # root dir (1 cluster)

    root_entries = bytearray()

    # Volume label entry
    root_entries += b"ARCADEOS   " + bytes([0x08]) + bytes(20)

    next_cluster = 3
    file_layouts = []   # (first_cluster, data)

    for path in files:
        with open(path, "rb") as f:
            data = f.read()

        clusters_needed = max(1, (len(data) + SPC * SECTOR - 1) // (SPC * SECTOR))
        first = next_cluster
        for i in range(clusters_needed):
            cur = next_cluster + i
            fat[cur] = cur + 1 if i < clusters_needed - 1 else 0x0FFFFFFF
        next_cluster += clusters_needed
        if next_cluster >= total_clusters + 2:
            print(f"mkfat32: ERROR: volume too small for {path}")
            sys.exit(1)

        entry = bytearray(32)
        entry[0:11] = short_name(path)
        entry[11] = 0x20                              # ATTR_ARCHIVE
        struct.pack_into("<H", entry, 20, first >> 16)
        struct.pack_into("<H", entry, 26, first & 0xFFFF)
        struct.pack_into("<I", entry, 28, len(data))
        root_entries += entry

        file_layouts.append((first, data))
        print(f"mkfat32: {os.path.basename(path)} -> cluster {first} "
              f"({len(data)} bytes, {clusters_needed} clusters)")

    # ---- Write the image ----
    with open(img_path, "wb") as f:
        f.truncate(total_sectors * SECTOR)

        f.seek(0)
        f.write(bpb)
        f.seek(1 * SECTOR)
        f.write(fsinfo)
        f.seek(6 * SECTOR)
        f.write(bpb)                                  # backup boot sector

        fat_bytes = struct.pack(f"<{len(fat)}I", *fat)
        for i in range(NUM_FATS):
            f.seek((RESERVED + i * fat_size) * SECTOR)
            f.write(fat_bytes)

        def cluster_offset(cluster):
            return (data_start + (cluster - 2) * SPC) * SECTOR

        # Root directory (cluster 2)
        f.seek(cluster_offset(2))
        f.write(root_entries)

        # File data
        for first, data in file_layouts:
            f.seek(cluster_offset(first))
            f.write(data)

    print(f"mkfat32: created {img_path} "
          f"({size_mb} MiB FAT32, {len(files)} files in root)")


if __name__ == "__main__":
    main()
