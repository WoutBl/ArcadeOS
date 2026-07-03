/*
 * ArcadeOS – ELF64 verification and parser
 */

#include "elf.h"

bool elf_verify(Elf64_Ehdr* header) {
    if (!header) return false;

    /* Verify Magic Number '\x7fELF' */
    if (memcmp(header->e_ident, ELF_MAGIC, 4) != 0) {
        return false;
    }

    /* Verify architecture class is 64-bit */
    if (header->e_ident[4] != ELFCLASS64) {
        return false;
    }

    /* Verify executable type */
    if (header->e_type != ET_EXEC) {
        return false;
    }

    /* Verify machine architecture is x86-64 */
    if (header->e_machine != EM_X86_64) {
        return false;
    }

    /* Verify ELF version */
    if (header->e_version != 1) {
        return false;
    }

    return true;
}
