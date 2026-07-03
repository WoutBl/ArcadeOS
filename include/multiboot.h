#ifndef MULTIBOOT_H
#define MULTIBOOT_H

#include "types.h"

/* Multiboot magic value placed in EAX by GRUB */
#define MULTIBOOT_MAGIC 0x2BADB002

/* Flags in multiboot_info_t.flags indicating which fields are valid */
#define MULTIBOOT_FLAG_MEM     (1 << 0)   /* mem_lower / mem_upper valid */
#define MULTIBOOT_FLAG_MMAP    (1 << 6)   /* mmap_length / mmap_addr valid */
#define MULTIBOOT_FLAG_FB      (1 << 12)  /* framebuffer_* fields valid */

/* Framebuffer types */
#define MULTIBOOT_FB_TYPE_INDEXED  0
#define MULTIBOOT_FB_TYPE_RGB      1
#define MULTIBOOT_FB_TYPE_EGA_TEXT 2

/* The Multiboot info structure passed by GRUB (at address in EBX) */
typedef struct {
    uint32_t flags;
    uint32_t mem_lower;        /* KiB of lower memory (below 1 MiB) */
    uint32_t mem_upper;        /* KiB of upper memory (above 1 MiB) */
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];          /* ELF section headers (unused) */
    uint32_t mmap_length;      /* Total size of memory map buffer */
    uint32_t mmap_addr;        /* Physical address of first mmap entry */
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;

    /* Framebuffer info (valid when MULTIBOOT_FLAG_FB is set) */
    uint32_t framebuffer_addr_low;   /* Physical address of the LFB (low 32) */
    uint32_t framebuffer_addr_high;  /* High 32 bits (0 on 32-bit machines) */
    uint32_t framebuffer_pitch;      /* Bytes per scanline */
    uint32_t framebuffer_width;      /* Pixels */
    uint32_t framebuffer_height;     /* Pixels */
    uint8_t  framebuffer_bpp;        /* Bits per pixel */
    uint8_t  framebuffer_type;       /* MULTIBOOT_FB_TYPE_* */
    uint8_t  framebuffer_red_field_position;
    uint8_t  framebuffer_red_mask_size;
    uint8_t  framebuffer_green_field_position;
    uint8_t  framebuffer_green_mask_size;
    uint8_t  framebuffer_blue_field_position;
    uint8_t  framebuffer_blue_mask_size;
} __attribute__((packed)) multiboot_info_t;

/* A single entry in the memory map */
typedef struct {
    uint32_t size;       /* Size of this entry (minus 4 for this field itself) */
    uint32_t base_low;   /* Base address (low 32 bits) */
    uint32_t base_high;  /* Base address (high 32 bits) */
    uint32_t length_low; /* Length in bytes (low 32 bits) */
    uint32_t length_high;/* Length in bytes (high 32 bits) */
    uint32_t type;       /* 1=available, 2=reserved, 3=ACPI reclaimable, etc. */
} __attribute__((packed)) multiboot_mmap_entry_t;

/* Memory map types */
#define MULTIBOOT_MMAP_AVAILABLE        1
#define MULTIBOOT_MMAP_RESERVED         2
#define MULTIBOOT_MMAP_ACPI_RECLAIMABLE 3
#define MULTIBOOT_MMAP_NVS              4
#define MULTIBOOT_MMAP_BADRAM           5

#endif /* MULTIBOOT_H */
