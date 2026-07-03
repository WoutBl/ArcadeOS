#ifndef ELF_H
#define ELF_H

#include "types.h"
#include <stdbool.h>

/* ELF Magic numbers */
#define ELF_MAGIC "\x7f\x45\x4c\x46"

/* ELF Types */
#define ET_NONE   0
#define ET_REL    1
#define ET_EXEC   2

/* Machine Architecture */
#define EM_386    3

/* Program Header Types */
#define PT_NULL   0
#define PT_LOAD   1

/* Program Header Flags */
#define PF_X 1   /* Executable */
#define PF_W 2   /* Writable */
#define PF_R 4   /* Readable */

/* 32-bit ELF Data Types */
typedef uint32_t Elf32_Addr;
typedef uint16_t Elf32_Half;
typedef uint32_t Elf32_Off;
typedef int32_t  Elf32_Sword;
typedef uint32_t Elf32_Word;

/* ELF 32-bit Header */
typedef struct {
    unsigned char e_ident[16];   /* Magic number and other info */
    Elf32_Half    e_type;        /* Object file type */
    Elf32_Half    e_machine;     /* Architecture */
    Elf32_Word    e_version;     /* Object file version */
    Elf32_Addr    e_entry;       /* Entry point virtual address */
    Elf32_Off     e_phoff;       /* Program header table file offset */
    Elf32_Off     e_shoff;       /* Section header table file offset */
    Elf32_Word    e_flags;       /* Processor-specific flags */
    Elf32_Half    e_ehsize;      /* ELF header size in bytes */
    Elf32_Half    e_phentsize;   /* Program header table entry size */
    Elf32_Half    e_phnum;       /* Program header table entry count */
    Elf32_Half    e_shentsize;   /* Section header table entry size */
    Elf32_Half    e_shnum;       /* Section header table entry count */
    Elf32_Half    e_shstrndx;    /* Section header string table index */
} __attribute__((packed)) Elf32_Ehdr;

/* ELF 32-bit Program Header */
typedef struct {
    Elf32_Word    p_type;        /* Segment type */
    Elf32_Off     p_offset;      /* Segment file offset */
    Elf32_Addr    p_vaddr;       /* Segment virtual address */
    Elf32_Addr    p_paddr;       /* Segment physical address */
    Elf32_Word    p_filesz;      /* Segment size in file */
    Elf32_Word    p_memsz;       /* Segment size in memory */
    Elf32_Word    p_flags;       /* Segment flags */
    Elf32_Word    p_align;       /* Segment alignment */
} __attribute__((packed)) Elf32_Phdr;

/* Public API */
bool elf_verify(Elf32_Ehdr* header);

#endif /* ELF_H */
