#ifndef FAT32_H
#define FAT32_H

#include "types.h"
#include "vfs.h"

/*
 * ArcadeOS – FAT32 Driver
 *
 * Reads the game volume from the ATA disk (superfloppy layout: the BPB
 * sits at LBA 0, no MBR). Mounted into the VFS at /games so the console
 * can load game ELF binaries and assets:
 *
 *     vfs_open("/games/PONG.ELF", 0)
 *
 * Only 8.3 short names are supported (the mkfat32.py tool generates
 * compliant volumes). Name comparison is case-insensitive.
 */

/* Probe the ATA disk for a FAT32 BPB. Returns 1 if a volume was found. */
int fat32_init(void);

int fat32_available(void);

/* Root directory node, ready to be passed to vfs_mount() */
vfs_node_t* fat32_get_root(void);

/*
 * Save-data API ("memory card" semantics, backs SYS_SAVE / SYS_LOAD).
 * Whole-file replace/read of an 8.3-named file in the volume root.
 * fat32_save returns 0 on success; fat32_load returns bytes read or -1.
 */
int fat32_save(const char* name, const uint8_t* data, uint32_t len);
int fat32_load(const char* name, uint8_t* out, uint32_t maxlen);

/* Nonzero while a save/load operation is in flight (single-CPU guard
 * used by the kernel log flusher to avoid interleaved volume writes). */
int fat32_busy(void);

#endif /* FAT32_H */
