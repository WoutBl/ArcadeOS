#ifndef FS_H
#define FS_H

#include "types.h"
#include "vfs.h"

/* Filesystem magic */
#define FS_MAGIC              0x4D494E44  /* "MIND" */
#define FS_SUPERBLOCK_SECTOR  0
#define FS_DATA_START_SECTOR  1
#define FS_SECTORS_PER_FILE   18

/* Superblock (512 bytes) */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t total_files;
    uint32_t reserved[125];
} fs_superblock_t;

/* File entry */
typedef struct {
    char   name[MAX_FILENAME];    /* 64  */
    char   content[MAX_FILE_SIZE]; /* 512 */
    char   path[MAX_PATH];        /* 128 */
    size_t size;                  /* 4   */
    int    in_use;                /* 4   */
    int    is_directory;          /* 4   */
} file_t;

/* Filesystem array */
extern file_t filesystem[];

/* Public API */
void    fs_initialize(void);
void    fs_init_disk(void);
int     fs_create_file(const char* filename, const char* content);
int     fs_create_binary_file(const char* filename, const uint8_t* content, size_t size);
int     fs_create_directory(const char* dirname);
file_t* fs_get_file(const char* filename);
file_t* fs_get_entry(const char* path);
int     fs_change_directory(const char* path);
int     fs_edit_file(const char* filename, const char* new_content);
int     fs_delete_file(const char* filename);
int     fs_copy_file(const char* src, const char* dest);
int     fs_move_file(const char* src, const char* dest);
int     fs_touch_file(const char* filename);
void    fs_find_files(const char* pattern);
int     fs_get_file_count(void);
int     fs_get_dir_count(void);
size_t  fs_get_total_size(void);
void    fs_list_files(void);
void    fs_save_to_disk(void);
void    fs_load_from_disk(void);

/* Returns a VFS node wrapping the legacy filesystem, for mounting at "/" */
vfs_node_t* fs_as_vfs_root(void);

/* Path helpers */
void path_join(char* result, const char* dir, const char* name);
int  path_is_absolute(const char* path);
void path_normalize(char* result, const char* current, const char* path);

#endif /* FS_H */
