/*
 * ArcadeOS – Filesystem (with 2-sector-per-file persistence)
 */

#include "fs.h"
#include "ata.h"
#include "vga.h"

/* ──────── Globals ──────── */
file_t filesystem[MAX_FILES];

/* ──────── Path helpers ──────── */
void path_join(char* result, const char* dir, const char* name) {
    strcpy(result, dir);
    size_t len = strlen(result);

    if (len > 0 && result[len - 1] != '/') {
        result[len] = '/';
        result[len + 1] = '\0';
    }

    size_t i = strlen(result);
    size_t j = 0;
    while (name[j] && i < MAX_PATH - 1)
        result[i++] = name[j++];
    result[i] = '\0';
}

int path_is_absolute(const char* path) {
    return path[0] == '/';
}

void path_normalize(char* result, const char* current, const char* path) {
    if (path_is_absolute(path))
        strcpy(result, path);
    else
        path_join(result, current, path);
}

/* ──────── Filesystem operations ──────── */
void fs_initialize(void) {
    for (int i = 0; i < MAX_FILES; i++) {
        filesystem[i].in_use = 0;
        filesystem[i].size = 0;
        filesystem[i].is_directory = 0;
    }

    strcpy(filesystem[0].name, "/");
    strcpy(filesystem[0].path, "/");
    filesystem[0].is_directory = 1;
    filesystem[0].in_use = 1;
    filesystem[0].size = 0;
}

int fs_create_file(const char* filename, const char* content) {
    char full_path[MAX_PATH];
    path_normalize(full_path, current_dir, filename);

    for (int i = 0; i < MAX_FILES; i++) {
        if (!filesystem[i].in_use) {
            const char* name_start = full_path;
            for (size_t j = strlen(full_path); j > 0; j--) {
                if (full_path[j - 1] == '/') {
                    name_start = full_path + j;
                    break;
                }
            }

            strcpy(filesystem[i].name, name_start);
            strcpy(filesystem[i].path, full_path);
            strcpy(filesystem[i].content, content);
            filesystem[i].size = strlen(content);
            filesystem[i].in_use = 1;
            filesystem[i].is_directory = 0;

            if (ata_is_present())
                fs_save_to_disk();

            return i;
        }
    }
    return -1;
}

int fs_create_binary_file(const char* filename, const uint8_t* content, size_t size) {
    if (size > MAX_FILE_SIZE) return -2;

    char full_path[MAX_PATH];
    path_normalize(full_path, current_dir, filename);

    for (int i = 0; i < MAX_FILES; i++) {
        if (!filesystem[i].in_use) {
            const char* name_start = full_path;
            for (size_t j = strlen(full_path); j > 0; j--) {
                if (full_path[j - 1] == '/') {
                    name_start = full_path + j;
                    break;
                }
            }

            strcpy(filesystem[i].name, name_start);
            strcpy(filesystem[i].path, full_path);
            
            memset(filesystem[i].content, 0, MAX_FILE_SIZE);
            memcpy(filesystem[i].content, content, size);
            
            filesystem[i].size = size;
            filesystem[i].in_use = 1;
            filesystem[i].is_directory = 0;

            if (ata_is_present())
                fs_save_to_disk();

            return i;
        }
    }
    return -1;
}

int fs_create_directory(const char* dirname) {
    char full_path[MAX_PATH];
    path_normalize(full_path, current_dir, dirname);

    for (int i = 0; i < MAX_FILES; i++) {
        if (filesystem[i].in_use && strcmp(filesystem[i].path, full_path) == 0)
            return -2;
    }

    for (int i = 0; i < MAX_FILES; i++) {
        if (!filesystem[i].in_use) {
            const char* name_start = full_path;
            for (size_t j = strlen(full_path); j > 0; j--) {
                if (full_path[j - 1] == '/') {
                    name_start = full_path + j;
                    break;
                }
            }

            strcpy(filesystem[i].name, name_start);
            strcpy(filesystem[i].path, full_path);
            filesystem[i].size = 0;
            filesystem[i].in_use = 1;
            filesystem[i].is_directory = 1;

            if (ata_is_present())
                fs_save_to_disk();

            return i;
        }
    }
    return -1;
}

file_t* fs_get_file(const char* filename) {
    char full_path[MAX_PATH];
    path_normalize(full_path, current_dir, filename);

    for (int i = 0; i < MAX_FILES; i++) {
        if (filesystem[i].in_use && !filesystem[i].is_directory &&
            strcmp(filesystem[i].path, full_path) == 0)
            return &filesystem[i];
    }
    return NULL;
}

file_t* fs_get_entry(const char* path) {
    char full_path[MAX_PATH];
    path_normalize(full_path, current_dir, path);

    for (int i = 0; i < MAX_FILES; i++) {
        if (filesystem[i].in_use && strcmp(filesystem[i].path, full_path) == 0)
            return &filesystem[i];
    }
    return NULL;
}

int fs_change_directory(const char* path) {
    char full_path[MAX_PATH];

    if (strcmp(path, "..") == 0) {
        size_t len = strlen(current_dir);
        if (len > 1) {
            if (current_dir[len - 1] == '/') {
                current_dir[len - 1] = '\0';
                len--;
            }
            for (size_t i = len; i > 0; i--) {
                if (current_dir[i - 1] == '/') {
                    if (i == 1)
                        current_dir[1] = '\0';
                    else
                        current_dir[i - 1] = '\0';
                    return 0;
                }
            }
        }
        return 0;
    }

    path_normalize(full_path, current_dir, path);

    for (int i = 0; i < MAX_FILES; i++) {
        if (filesystem[i].in_use && filesystem[i].is_directory &&
            strcmp(filesystem[i].path, full_path) == 0) {
            strcpy(current_dir, full_path);
            return 0;
        }
    }
    return -1;
}

int fs_edit_file(const char* filename, const char* new_content) {
    char full_path[MAX_PATH];
    path_normalize(full_path, current_dir, filename);

    for (int i = 0; i < MAX_FILES; i++) {
        if (filesystem[i].in_use && !filesystem[i].is_directory &&
            strcmp(filesystem[i].path, full_path) == 0) {
            strcpy(filesystem[i].content, new_content);
            filesystem[i].size = strlen(new_content);

            if (ata_is_present())
                fs_save_to_disk();
            return 0;
        }
    }
    return -1;
}

int fs_delete_file(const char* filename) {
    char full_path[MAX_PATH];
    path_normalize(full_path, current_dir, filename);

    for (int i = 0; i < MAX_FILES; i++) {
        if (filesystem[i].in_use && strcmp(filesystem[i].path, full_path) == 0) {
            filesystem[i].in_use = 0;
            filesystem[i].size = 0;

            if (ata_is_present())
                fs_save_to_disk();
            return 0;
        }
    }
    return -1;
}

int fs_copy_file(const char* src, const char* dest) {
    file_t* src_file = fs_get_file(src);
    if (!src_file) return -1;
    if (src_file->size >= MAX_FILE_SIZE) return -3;

    int result = fs_create_file(dest, src_file->content);
    return (result < 0) ? -2 : 0;
}

int fs_move_file(const char* src, const char* dest) {
    char src_full_path[MAX_PATH];
    path_normalize(src_full_path, current_dir, src);

    file_t* src_file = NULL;
    int src_index = -1;
    for (int i = 0; i < MAX_FILES; i++) {
        if (filesystem[i].in_use && strcmp(filesystem[i].path, src_full_path) == 0) {
            src_file = &filesystem[i];
            src_index = i;
            break;
        }
    }
    if (!src_file) return -1;

    char dest_full_path[MAX_PATH];
    path_normalize(dest_full_path, current_dir, dest);

    char dest_dir[MAX_PATH];
    strcpy(dest_dir, dest_full_path);
    int last_slash_pos = -1;
    for (size_t j = 0; dest_dir[j]; j++) {
        if (dest_dir[j] == '/')
            last_slash_pos = (int)j;
    }

    if (last_slash_pos > 0) {
        dest_dir[last_slash_pos] = '\0';
        int dir_exists = 0;
        for (int i = 0; i < MAX_FILES; i++) {
            if (filesystem[i].in_use && filesystem[i].is_directory &&
                strcmp(filesystem[i].path, dest_dir) == 0) {
                dir_exists = 1;
                break;
            }
        }
        if (!dir_exists) {
            const char* dir_name_start = dest_dir;
            for (size_t j = strlen(dest_dir); j > 0; j--) {
                if (dest_dir[j - 1] == '/') {
                    dir_name_start = dest_dir + j;
                    break;
                }
            }
            int found_slot = 0;
            for (int i = 0; i < MAX_FILES; i++) {
                if (!filesystem[i].in_use) {
                    strcpy(filesystem[i].name, dir_name_start);
                    strcpy(filesystem[i].path, dest_dir);
                    filesystem[i].size = 0;
                    filesystem[i].in_use = 1;
                    filesystem[i].is_directory = 1;
                    found_slot = 1;
                    break;
                }
            }
            if (!found_slot) return -2;
        }
    }

    const char* name_start = dest_full_path;
    for (size_t j = strlen(dest_full_path); j > 0; j--) {
        if (dest_full_path[j - 1] == '/') {
            name_start = dest_full_path + j;
            break;
        }
    }

    strcpy(filesystem[src_index].name, name_start);
    strcpy(filesystem[src_index].path, dest_full_path);
    return 0;
}

int fs_touch_file(const char* filename) {
    file_t* existing = fs_get_file(filename);
    if (existing) return 0;
    return fs_create_file(filename, "");
}

void fs_find_files(const char* pattern) {
    int found = 0;

    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("Searching for: ");
    terminal_writestring(pattern);
    terminal_writestring("\n");

    for (int i = 0; i < MAX_FILES; i++) {
        if (filesystem[i].in_use) {
            int match = 0;
            const char* name = filesystem[i].name;
            for (size_t j = 0; name[j]; j++) {
                int found_match = 1;
                for (size_t k = 0; pattern[k]; k++) {
                    if (name[j + k] != pattern[k]) {
                        found_match = 0;
                        break;
                    }
                }
                if (found_match && pattern[0] != '\0') {
                    match = 1;
                    break;
                }
            }
            if (match) {
                if (filesystem[i].is_directory) {
                    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_BLUE, VGA_COLOR_BLACK));
                    terminal_writestring("  [DIR]  ");
                } else {
                    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
                    terminal_writestring("  [FILE] ");
                }
                terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
                terminal_writestring(filesystem[i].path);
                terminal_writestring("\n");
                found = 1;
            }
        }
    }
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
    if (!found)
        terminal_writestring("  No matches found\n");
}

int fs_get_file_count(void) {
    int count = 0;
    for (int i = 0; i < MAX_FILES; i++)
        if (filesystem[i].in_use && !filesystem[i].is_directory) count++;
    return count;
}

int fs_get_dir_count(void) {
    int count = 0;
    for (int i = 0; i < MAX_FILES; i++)
        if (filesystem[i].in_use && filesystem[i].is_directory) count++;
    return count;
}

size_t fs_get_total_size(void) {
    size_t total = 0;
    for (int i = 0; i < MAX_FILES; i++)
        if (filesystem[i].in_use) total += filesystem[i].size;
    return total;
}

void fs_list_files(void) {
    int found = 0;
    size_t current_len = strlen(current_dir);

    for (int i = 0; i < MAX_FILES; i++) {
        if (filesystem[i].in_use) {
            int in_current = 1;
            for (size_t j = 0; j < current_len; j++) {
                if (filesystem[i].path[j] != current_dir[j]) {
                    in_current = 0;
                    break;
                }
            }

            if (in_current) {
                const char* remainder = filesystem[i].path + current_len;
                if (*remainder == '/') remainder++;

                int is_direct_child = 1;
                for (size_t j = 0; remainder[j]; j++) {
                    if (remainder[j] == '/' && remainder[j + 1] != '\0') {
                        is_direct_child = 0;
                        break;
                    }
                }

                if (strcmp(filesystem[i].path, current_dir) == 0) continue;

                if (is_direct_child && strlen(remainder) > 0) {
                    if (filesystem[i].is_directory) {
                        terminal_setcolor(vga_entry_color(themes[current_theme].dir_fg, themes[current_theme].text_bg));
                        terminal_writestring("  [DIR]  ");
                    } else {
                        const char* ext = strrchr(filesystem[i].name, '.');
                        if (ext && strcmp(ext, ".mos") == 0)
                            terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, themes[current_theme].text_bg));
                        else if (ext && (strcmp(ext, ".txt") == 0 || strcmp(ext, ".md") == 0))
                            terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, themes[current_theme].text_bg));
                        else
                            terminal_setcolor(vga_entry_color(themes[current_theme].file_fg, themes[current_theme].text_bg));
                        terminal_writestring("  [FILE] ");
                    }
                    terminal_writestring(filesystem[i].name);
                    terminal_writestring("\n");
                    found = 1;
                }
            }
        }
    }

    terminal_setcolor(vga_entry_color(themes[current_theme].text_fg, themes[current_theme].text_bg));
    if (!found)
        terminal_writestring("  (empty)\n");
}

/* ──────── Disk persistence (2-sector-per-file) ──────── */
void fs_init_disk(void) {
    /*
     * ArcadeOS: the raw ATA disk is a FAT32 game volume (fat32.c).
     * The legacy filesystem is RAM-only now; persisting it here
     * would overwrite the FAT32 boot sector and corrupt the volume.
     */
}

void fs_save_to_disk(void) {
    /*
     * ArcadeOS: the raw ATA disk is a FAT32 game volume (fat32.c).
     * The legacy filesystem is RAM-only now; persisting it here
     * would overwrite the FAT32 boot sector and corrupt the volume.
     */
}

void fs_load_from_disk(void) {
    /*
     * ArcadeOS: the raw ATA disk is a FAT32 game volume (fat32.c).
     * The legacy filesystem is RAM-only now; persisting it here
     * would overwrite the FAT32 boot sector and corrupt the volume.
     */
}

/* ──────── VFS wrapper for the legacy filesystem ──────── */

/*
 * VFS read: reads a file's content into buf.
 * node->impl_data points to the file_t entry.
 * offset is respected; size = bytes to copy.
 */
static int32_t fs_vfs_read(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buf) {
    file_t* f = (file_t*)node->impl_data;
    if (!f || !f->in_use || f->is_directory) return -1;

    uint32_t fsize = (uint32_t)f->size;
    if (offset >= fsize) return 0;
    uint32_t avail = fsize - offset;
    uint32_t to_copy = (size < avail) ? size : avail;

    for (uint32_t i = 0; i < to_copy; i++) {
        buf[i] = (uint8_t)f->content[offset + i];
    }
    return (int32_t)to_copy;
}

/*
 * VFS finddir: given the legacy root, find a file_t by name.
 * Returns a statically allocated node (we keep a small per-lookup pool).
 */
#define FS_VFS_POOL 8
static vfs_node_t fs_vfs_file_pool[FS_VFS_POOL];
static int        fs_vfs_pool_idx = 0;

static vfs_node_t* fs_vfs_finddir(vfs_node_t* dir_node __attribute__((unused)),
                                   const char* name) {
    file_t* found = fs_get_file(name);
    if (!found || !found->in_use) return (vfs_node_t*)0;

    vfs_node_t* n = &fs_vfs_file_pool[fs_vfs_pool_idx % FS_VFS_POOL];
    fs_vfs_pool_idx++;

    memset(n, 0, sizeof(vfs_node_t));
    strncpy(n->name, found->name, sizeof(n->name) - 1);
    n->flags     = found->is_directory ? VFS_FLAG_DIR : VFS_FLAG_FILE;
    n->length    = (uint32_t)found->size;
    n->inode     = (uint32_t)(found - filesystem);
    n->impl_data = (void*)found;
    n->read      = fs_vfs_read;
    n->finddir   = fs_vfs_finddir;

    return n;
}

/* The single root node for the legacy filesystem */
static vfs_node_t fs_root_node;

vfs_node_t* fs_as_vfs_root(void) {
    memset(&fs_root_node, 0, sizeof(vfs_node_t));
    strncpy(fs_root_node.name, "root", sizeof(fs_root_node.name) - 1);
    fs_root_node.flags   = VFS_FLAG_DIR | VFS_FLAG_MOUNT;
    fs_root_node.inode   = 0;
    fs_root_node.finddir = fs_vfs_finddir;
    return &fs_root_node;
}
