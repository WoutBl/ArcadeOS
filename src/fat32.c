/*
 * ArcadeOS – FAT32 Driver
 *
 * Read support for game/asset loading plus a whole-file write path
 * (fat32_save/fat32_load) used by the save-data syscalls. Sits directly
 * on the ATA PIO driver and exposes the volume through vfs_node_t
 * operations. Root directory, 8.3 names only.
 */

#include "fat32.h"
#include "disk.h"
#include "vga.h"
#include "heap.h"

/* ──────── On-disk structures ──────── */

typedef struct {
    uint8_t  jmp[3];
    char     oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entries_16;     /* 0 on FAT32 */
    uint16_t total_sectors_16;    /* 0 on FAT32 */
    uint8_t  media;
    uint16_t fat_size_16;         /* 0 on FAT32 */
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    /* FAT32 extension */
    uint32_t fat_size_32;         /* Sectors per FAT */
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fsinfo_sector;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_signature;
    uint32_t volume_id;
    char     volume_label[11];
    char     fs_type[8];          /* "FAT32   " */
} __attribute__((packed)) fat32_bpb_t;

typedef struct {
    char     name[11];            /* 8.3, space-padded */
    uint8_t  attr;
    uint8_t  nt_reserved;
    uint8_t  create_time_tenth;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t access_date;
    uint16_t first_cluster_high;
    uint16_t write_time;
    uint16_t write_date;
    uint16_t first_cluster_low;
    uint32_t size;
} __attribute__((packed)) fat32_dirent_t;

#define FAT_ATTR_READ_ONLY 0x01
#define FAT_ATTR_HIDDEN    0x02
#define FAT_ATTR_SYSTEM    0x04
#define FAT_ATTR_VOLUME_ID 0x08
#define FAT_ATTR_DIRECTORY 0x10
#define FAT_ATTR_LFN       0x0F   /* Long file name entry (skipped) */

#define FAT32_EOC          0x0FFFFFF8   /* End-of-chain marker (>=) */

/* ──────── Volume state ──────── */

static int      fat_present = 0;
static uint32_t fat_start_lba;        /* First FAT sector */
static uint32_t data_start_lba;       /* First data-region sector */
static uint32_t sectors_per_cluster;
static uint32_t bytes_per_cluster;
static uint32_t root_cluster;
static uint32_t total_clusters;       /* Data-region clusters (write support) */
static uint32_t fat_size_sectors;     /* Sectors per FAT (write support) */
static uint32_t num_fats;

/* Single-sector FAT cache (cluster chains are walked sector by sector) */
static uint8_t  fat_cache[DISK_SECTOR_SIZE];
static uint32_t fat_cache_lba = 0xFFFFFFFF;

/* Sector bounce buffer for data reads */
static uint8_t sector_buf[DISK_SECTOR_SIZE];

/* VFS node pool (nodes handed out by finddir; recycled round-robin) */
#define FAT32_NODE_POOL 32
static vfs_node_t   node_pool[FAT32_NODE_POOL];
static int          node_pool_next = 0;
static vfs_node_t   fat_root_node;
static vfs_dirent_t readdir_out;

/* ──────── FAT access ──────── */

static uint32_t fat_next_cluster(uint32_t cluster) {
    uint32_t fat_offset = cluster * 4;
    uint32_t lba        = fat_start_lba + fat_offset / DISK_SECTOR_SIZE;
    uint32_t entry_off  = fat_offset % DISK_SECTOR_SIZE;

    if (lba != fat_cache_lba) {
        if (!disk_read_sector(lba, fat_cache)) return FAT32_EOC;
        fat_cache_lba = lba;
    }
    uint32_t value;
    memcpy(&value, &fat_cache[entry_off], 4);
    return value & 0x0FFFFFFF;
}

static uint32_t cluster_to_lba(uint32_t cluster) {
    return data_start_lba + (cluster - 2) * sectors_per_cluster;
}

/* ──────── 8.3 name handling ──────── */

static char to_upper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

/* "PONG.ELF" → "PONG    ELF" (11 bytes, space-padded, uppercased) */
static void format_83(const char* name, char out[11]) {
    memset(out, ' ', 11);

    int i = 0, pos = 0;
    while (name[i] && name[i] != '.' && pos < 8)
        out[pos++] = to_upper(name[i++]);
    while (name[i] && name[i] != '.')
        i++;   /* Skip overlong base names */
    if (name[i] == '.') {
        i++;
        pos = 8;
        while (name[i] && pos < 11)
            out[pos++] = to_upper(name[i++]);
    }
}

static int name_matches(const fat32_dirent_t* de, const char* name) {
    char formatted[11];
    format_83(name, formatted);
    return memcmp(formatted, de->name, 11) == 0;
}

/* Raw 11-byte entry → "PONG.ELF" (null-terminated, max 13 chars) */
static void name_from_entry(const fat32_dirent_t* de, char* out) {
    int pos = 0;
    for (int i = 0; i < 8 && de->name[i] != ' '; i++)
        out[pos++] = de->name[i];
    if (de->name[8] != ' ') {
        out[pos++] = '.';
        for (int i = 8; i < 11 && de->name[i] != ' '; i++)
            out[pos++] = de->name[i];
    }
    out[pos] = '\0';
}

/* ──────── Directory iteration ──────── */

/*
 * Walk the directory starting at 'dir_cluster' and invoke the visitor
 * for each valid entry. The visitor returns non-zero to stop early.
 */
typedef int (*dirent_visitor_t)(const fat32_dirent_t* de, void* ctx);

static int fat32_iterate_dir(uint32_t dir_cluster, dirent_visitor_t visit, void* ctx) {
    uint32_t cluster = dir_cluster;

    while (cluster >= 2 && cluster < FAT32_EOC) {
        uint32_t lba = cluster_to_lba(cluster);

        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            if (!disk_read_sector(lba + s, sector_buf)) return 0;

            fat32_dirent_t* entries = (fat32_dirent_t*)sector_buf;
            for (uint32_t e = 0; e < DISK_SECTOR_SIZE / sizeof(fat32_dirent_t); e++) {
                fat32_dirent_t* de = &entries[e];

                if ((uint8_t)de->name[0] == 0x00) return 0;   /* End of directory */
                if ((uint8_t)de->name[0] == 0xE5) continue;   /* Deleted */
                if ((de->attr & FAT_ATTR_LFN) == FAT_ATTR_LFN) continue;
                if (de->attr & FAT_ATTR_VOLUME_ID) continue;

                if (visit(de, ctx)) return 1;
            }
        }
        cluster = fat_next_cluster(cluster);
    }
    return 0;
}

/* ──────── VFS operations ──────── */

static int32_t fat32_vfs_read(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buf) {
    if (!node || !buf) return -1;
    if (offset >= node->length) return 0;
    if (offset + size > node->length) size = node->length - offset;

    uint32_t first_cluster = node->inode;
    uint32_t cluster = first_cluster;

    /* Skip whole clusters before the offset */
    uint32_t skip = offset / bytes_per_cluster;
    for (uint32_t i = 0; i < skip && cluster < FAT32_EOC; i++)
        cluster = fat_next_cluster(cluster);

    uint32_t cluster_off = offset % bytes_per_cluster;
    uint32_t copied = 0;

    while (copied < size && cluster >= 2 && cluster < FAT32_EOC) {
        uint32_t lba = cluster_to_lba(cluster);

        for (uint32_t s = cluster_off / DISK_SECTOR_SIZE;
             s < sectors_per_cluster && copied < size; s++) {

            if (!disk_read_sector(lba + s, sector_buf)) return (int32_t)copied;

            uint32_t sec_off = (copied == 0) ? (cluster_off % DISK_SECTOR_SIZE) : 0;
            uint32_t avail   = DISK_SECTOR_SIZE - sec_off;
            uint32_t want    = size - copied;
            uint32_t n       = (want < avail) ? want : avail;

            memcpy(buf + copied, sector_buf + sec_off, n);
            copied += n;
        }

        cluster_off = 0;
        cluster = fat_next_cluster(cluster);
    }
    return (int32_t)copied;
}

static vfs_node_t* fat32_make_node(const fat32_dirent_t* de);

/* finddir visitor */
typedef struct {
    const char*  name;
    vfs_node_t*  result;
} finddir_ctx_t;

static int finddir_visitor(const fat32_dirent_t* de, void* ctx) {
    finddir_ctx_t* fc = (finddir_ctx_t*)ctx;
    if (name_matches(de, fc->name)) {
        fc->result = fat32_make_node(de);
        return 1;
    }
    return 0;
}

static vfs_node_t* fat32_vfs_finddir(vfs_node_t* node, const char* name) {
    if (!node || !name) return (vfs_node_t*)0;

    finddir_ctx_t ctx = { name, (vfs_node_t*)0 };
    fat32_iterate_dir(node->inode, finddir_visitor, &ctx);
    return ctx.result;
}

/* readdir visitor */
typedef struct {
    uint32_t       target;
    uint32_t       count;
    vfs_dirent_t*  result;
} readdir_ctx_t;

static int readdir_visitor(const fat32_dirent_t* de, void* ctx) {
    readdir_ctx_t* rc = (readdir_ctx_t*)ctx;
    if (rc->count == rc->target) {
        name_from_entry(de, readdir_out.name);
        readdir_out.inode = ((uint32_t)de->first_cluster_high << 16) | de->first_cluster_low;
        readdir_out.flags = (de->attr & FAT_ATTR_DIRECTORY) ? VFS_FLAG_DIR : VFS_FLAG_FILE;
        rc->result = &readdir_out;
        return 1;
    }
    rc->count++;
    return 0;
}

static vfs_dirent_t* fat32_vfs_readdir(vfs_node_t* node, uint32_t index) {
    if (!node) return (vfs_dirent_t*)0;

    readdir_ctx_t ctx = { index, 0, (vfs_dirent_t*)0 };
    fat32_iterate_dir(node->inode, readdir_visitor, &ctx);
    return ctx.result;
}

static vfs_node_t* fat32_make_node(const fat32_dirent_t* de) {
    vfs_node_t* node = &node_pool[node_pool_next];
    node_pool_next = (node_pool_next + 1) % FAT32_NODE_POOL;

    memset(node, 0, sizeof(*node));
    name_from_entry(de, node->name);
    node->inode  = ((uint32_t)de->first_cluster_high << 16) | de->first_cluster_low;
    node->length = de->size;

    if (de->attr & FAT_ATTR_DIRECTORY) {
        node->flags   = VFS_FLAG_DIR;
        node->readdir = fat32_vfs_readdir;
        node->finddir = fat32_vfs_finddir;
    } else {
        node->flags = VFS_FLAG_FILE;
        node->read  = fat32_vfs_read;
    }
    return node;
}

/* ──────── Public API ──────── */

int fat32_available(void) { return fat_present; }

vfs_node_t* fat32_get_root(void) {
    return fat_present ? &fat_root_node : (vfs_node_t*)0;
}

int fat32_init(void) {
    fat_present = 0;

    if (!disk_is_present()) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK));
        terminal_writestring("[FAT32] No disk - game volume unavailable\n");
        return 0;
    }

    static uint8_t bpb_buf[DISK_SECTOR_SIZE];
    if (!disk_read_sector(0, bpb_buf)) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("[FAT32] Failed to read boot sector\n");
        return 0;
    }

    fat32_bpb_t* bpb = (fat32_bpb_t*)bpb_buf;

    /* Sanity checks: 0x55AA signature + "FAT32" type string */
    if (bpb_buf[510] != 0x55 || bpb_buf[511] != 0xAA ||
        memcmp(bpb->fs_type, "FAT32   ", 8) != 0 ||
        bpb->bytes_per_sector != DISK_SECTOR_SIZE) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK));
        terminal_writestring("[FAT32] Disk is not a FAT32 volume\n");
        return 0;
    }

    sectors_per_cluster = bpb->sectors_per_cluster;
    bytes_per_cluster   = sectors_per_cluster * DISK_SECTOR_SIZE;
    fat_start_lba       = bpb->reserved_sectors;
    fat_size_sectors    = bpb->fat_size_32;
    num_fats            = bpb->num_fats;
    data_start_lba      = fat_start_lba + num_fats * fat_size_sectors;
    root_cluster        = bpb->root_cluster;
    total_clusters      = (bpb->total_sectors_32 - data_start_lba) / sectors_per_cluster;
    fat_cache_lba       = 0xFFFFFFFF;

    /* Build the root VFS node */
    memset(&fat_root_node, 0, sizeof(fat_root_node));
    strncpy(fat_root_node.name, "fat32", sizeof(fat_root_node.name) - 1);
    fat_root_node.flags   = VFS_FLAG_DIR | VFS_FLAG_MOUNT;
    fat_root_node.inode   = root_cluster;
    fat_root_node.readdir = fat32_vfs_readdir;
    fat_root_node.finddir = fat32_vfs_finddir;

    fat_present = 1;

    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("[FAT32] Volume mounted (");
    terminal_writedec(bpb->total_sectors_32 / 2048);
    terminal_writestring(" MiB, ");
    terminal_writedec(sectors_per_cluster);
    terminal_writestring(" sectors/cluster)\n");
    return 1;
}

/* ──────── Write support (save data) ────────
 *
 * Whole-file semantics with a crash-safe ordering: the new data is
 * written to a FRESH cluster chain while the old chain stays allocated
 * and referenced, and the single-sector directory-entry update is the
 * commit point (write-to-temp-then-rename, FAT-style). A power cut at
 * any moment leaves either the complete old file or the complete new
 * one — never a torn save. The cost: clusters written before an
 * interrupted commit leak as allocated-but-unreferenced (bounded by
 * one save file per crash; reclaimable by any fsck).
 */

/* Update a FAT entry in every FAT copy. Keeps the read cache coherent. */
static int fat_set(uint32_t cluster, uint32_t value) {
    uint32_t fat_offset = cluster * 4;
    uint32_t sec_idx    = fat_offset / DISK_SECTOR_SIZE;
    uint32_t entry_off  = fat_offset % DISK_SECTOR_SIZE;
    uint32_t lba0       = fat_start_lba + sec_idx;

    if (lba0 != fat_cache_lba) {
        if (!disk_read_sector(lba0, fat_cache)) return 0;
        fat_cache_lba = lba0;
    }

    uint32_t cur;
    memcpy(&cur, &fat_cache[entry_off], 4);
    cur = (cur & 0xF0000000) | (value & 0x0FFFFFFF);   /* Preserve reserved bits */
    memcpy(&fat_cache[entry_off], &cur, 4);

    for (uint32_t f = 0; f < num_fats; f++) {
        if (!disk_write_sector(fat_start_lba + f * fat_size_sectors + sec_idx, fat_cache))
            return 0;
    }
    return 1;
}

/* Find a free cluster, mark it end-of-chain, and return it (0 = full) */
static uint32_t fat_alloc_cluster(void) {
    for (uint32_t c = 2; c < total_clusters + 2; c++) {
        if (fat_next_cluster(c) == 0) {
            if (!fat_set(c, 0x0FFFFFFF)) return 0;
            return c;
        }
    }
    return 0;
}

static void fat_free_chain(uint32_t cluster) {
    while (cluster >= 2 && cluster < FAT32_EOC) {
        uint32_t next = fat_next_cluster(cluster);
        fat_set(cluster, 0);
        cluster = next;
    }
}

/* Location of a directory entry on disk, plus a copy of its contents */
typedef struct {
    uint32_t       lba;      /* Sector holding the entry */
    uint32_t       index;    /* Entry index within that sector */
    fat32_dirent_t de;
} dirent_loc_t;

/*
 * Walk the root directory. mode 0: find the entry named 'name'.
 * mode 1: find a free slot (0x00 terminator or 0xE5 deleted entry).
 * Returns 1 and fills 'loc' on success.
 */
static int root_dir_locate(int mode, const char* name, dirent_loc_t* loc) {
    char formatted[11];
    if (mode == 0) format_83(name, formatted);

    uint32_t cluster = root_cluster;
    while (cluster >= 2 && cluster < FAT32_EOC) {
        uint32_t lba = cluster_to_lba(cluster);

        for (uint32_t sec = 0; sec < sectors_per_cluster; sec++) {
            if (!disk_read_sector(lba + sec, sector_buf)) return 0;

            fat32_dirent_t* entries = (fat32_dirent_t*)sector_buf;
            for (uint32_t e = 0; e < DISK_SECTOR_SIZE / sizeof(fat32_dirent_t); e++) {
                fat32_dirent_t* de = &entries[e];
                uint8_t first = (uint8_t)de->name[0];

                if (mode == 1) {
                    if (first == 0x00 || first == 0xE5) {
                        loc->lba   = lba + sec;
                        loc->index = e;
                        loc->de    = *de;
                        return 1;
                    }
                    continue;
                }

                if (first == 0x00) return 0;   /* End of directory */
                if (first == 0xE5) continue;
                if ((de->attr & FAT_ATTR_LFN) == FAT_ATTR_LFN) continue;
                if (de->attr & FAT_ATTR_VOLUME_ID) continue;

                if (memcmp(formatted, de->name, 11) == 0) {
                    loc->lba   = lba + sec;
                    loc->index = e;
                    loc->de    = *de;
                    return 1;
                }
            }
        }
        cluster = fat_next_cluster(cluster);
    }
    return 0;
}

/* Write an updated directory entry back to disk */
static int dirent_writeback(const dirent_loc_t* loc) {
    if (!disk_read_sector(loc->lba, sector_buf)) return 0;
    memcpy(sector_buf + loc->index * sizeof(fat32_dirent_t), &loc->de,
           sizeof(fat32_dirent_t));
    return disk_write_sector(loc->lba, sector_buf);
}

/* ──────── Public save/load API (backs SYS_SAVE / SYS_LOAD) ──────── */

static void save_fail(const char* why) {
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
    terminal_writestring("[FAT32] save failed: ");
    terminal_writestring(why);
    terminal_writestring("\n");
}

static int fat32_save_impl(const char* name, const uint8_t* data, uint32_t len) {
    if (!fat_present || !name || (!data && len > 0)) { save_fail("args"); return -1; }

    /* Find the file, or create a fresh entry in a free root-dir slot.
     * The old chain is NOT freed yet — it must survive until the new
     * chain is committed, so an interrupted save keeps the old file. */
    dirent_loc_t loc;
    uint32_t old_first = 0;
    if (root_dir_locate(0, name, &loc)) {
        old_first = ((uint32_t)loc.de.first_cluster_high << 16)
                  | loc.de.first_cluster_low;
    } else {
        if (!root_dir_locate(1, name, &loc)) { save_fail("dir full"); return -1; }
        memset(&loc.de, 0, sizeof(loc.de));
        format_83(name, loc.de.name);
        loc.de.attr = 0x20;   /* ATTR_ARCHIVE */
    }

    /* Allocate a fresh chain and write the data, zero-padding the tail */
    uint32_t first_cluster = 0;
    uint32_t prev = 0;
    uint32_t clusters = (len + bytes_per_cluster - 1) / bytes_per_cluster;

    for (uint32_t i = 0; i < clusters; i++) {
        uint32_t c = fat_alloc_cluster();
        if (c == 0) {
            if (first_cluster) fat_free_chain(first_cluster);
            save_fail("volume full");
            return -1;
        }
        if (prev) fat_set(prev, c);
        else      first_cluster = c;
        prev = c;

        uint32_t base = i * bytes_per_cluster;
        for (uint32_t sec = 0; sec < sectors_per_cluster; sec++) {
            uint32_t off = base + sec * DISK_SECTOR_SIZE;
            memset(sector_buf, 0, DISK_SECTOR_SIZE);
            if (off < len) {
                uint32_t n = len - off;
                if (n > DISK_SECTOR_SIZE) n = DISK_SECTOR_SIZE;
                memcpy(sector_buf, data + off, n);
            }
            if (!disk_write_sector(cluster_to_lba(c) + sec, sector_buf)) {
                fat_free_chain(first_cluster);
                save_fail("data write");
                return -1;
            }
        }
    }

    /* Commit: point the directory entry at the new chain (one sector) */
    loc.de.first_cluster_low  = (uint16_t)(first_cluster & 0xFFFF);
    loc.de.first_cluster_high = (uint16_t)(first_cluster >> 16);
    loc.de.size = len;
    if (!dirent_writeback(&loc)) {
        if (first_cluster) fat_free_chain(first_cluster);
        save_fail("dirent writeback");
        return -1;
    }

    /* Only now is the old chain unreachable — release it */
    if (old_first >= 2)
        fat_free_chain(old_first);

    disk_flush();
    return 0;
}

static int fat32_load_impl(const char* name, uint8_t* out, uint32_t maxlen) {
    if (!fat_present || !name || !out) return -1;

    dirent_loc_t loc;
    if (!root_dir_locate(0, name, &loc)) return -1;

    uint32_t size = loc.de.size;
    if (size > maxlen) size = maxlen;

    /* Reuse the chain-walking reader through a stack node */
    vfs_node_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.inode  = ((uint32_t)loc.de.first_cluster_high << 16) | loc.de.first_cluster_low;
    tmp.length = loc.de.size;
    return fat32_vfs_read(&tmp, 0, size, out);
}

/*
 * ──────── Concurrency guard ────────
 *
 * fat32_busy() lets the kernel log flusher (which runs from the idle
 * task) avoid interleaving its whole-file write with a game's SYS_SAVE /
 * SYS_LOAD that was preempted mid-operation. Single CPU: the counter is
 * only ever observed under cli(), so a plain int is enough.
 */
static int fat32_op_depth = 0;

int fat32_busy(void) { return fat32_op_depth != 0; }

int fat32_save(const char* name, const uint8_t* data, uint32_t len) {
    fat32_op_depth++;
    int r = fat32_save_impl(name, data, len);
    fat32_op_depth--;
    return r;
}

int fat32_load(const char* name, uint8_t* out, uint32_t maxlen) {
    fat32_op_depth++;
    int r = fat32_load_impl(name, out, maxlen);
    fat32_op_depth--;
    return r;
}
