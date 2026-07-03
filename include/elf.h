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
#define EM_386     3
#define EM_X86_64 62

/* ELF class (e_ident[4]) */
#define ELFCLASS32 1
#define ELFCLASS64 2

/* Program Header Types */
#define PT_NULL   0
#define PT_LOAD   1

/* Program Header Flags */
#define PF_X 1   /* Executable */
#define PF_W 2   /* Writable */
#define PF_R 4   /* Readable */

/* 64-bit ELF Data Types */
typedef uint64_t Elf64_Addr;
typedef uint16_t Elf64_Half;
typedef uint64_t Elf64_Off;
typedef uint32_t Elf64_Word;
typedef uint64_t Elf64_Xword;

/* ELF 64-bit Header */
typedef struct {
    unsigned char e_ident[16];   /* Magic number and other info */
    Elf64_Half    e_type;        /* Object file type */
    Elf64_Half    e_machine;     /* Architecture */
    Elf64_Word    e_version;     /* Object file version */
    Elf64_Addr    e_entry;       /* Entry point virtual address */
    Elf64_Off     e_phoff;       /* Program header table file offset */
    Elf64_Off     e_shoff;       /* Section header table file offset */
    Elf64_Word    e_flags;       /* Processor-specific flags */
    Elf64_Half    e_ehsize;      /* ELF header size in bytes */
    Elf64_Half    e_phentsize;   /* Program header table entry size */
    Elf64_Half    e_phnum;       /* Program header table entry count */
    Elf64_Half    e_shentsize;   /* Section header table entry size */
    Elf64_Half    e_shnum;       /* Section header table entry count */
    Elf64_Half    e_shstrndx;    /* Section header string table index */
} __attribute__((packed)) Elf64_Ehdr;

/* ELF 64-bit Program Header (note: field order differs from ELF32) */
typedef struct {
    Elf64_Word    p_type;        /* Segment type */
    Elf64_Word    p_flags;       /* Segment flags */
    Elf64_Off     p_offset;      /* Segment file offset */
    Elf64_Addr    p_vaddr;       /* Segment virtual address */
    Elf64_Addr    p_paddr;       /* Segment physical address */
    Elf64_Xword   p_filesz;      /* Segment size in file */
    Elf64_Xword   p_memsz;       /* Segment size in memory */
    Elf64_Xword   p_align;       /* Segment alignment */
} __attribute__((packed)) Elf64_Phdr;

/* Public API */
bool elf_verify(Elf64_Ehdr* header);

#endif /* ELF_H */
