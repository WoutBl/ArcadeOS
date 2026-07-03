/*
 * ArcadeOS – ELF32 verification and parser
 */

#include "elf.h"

bool elf_verify(Elf32_Ehdr* header) {
    if (!header) return false;

    /* Verify Magic Number '\x7fELF' */
    if (memcmp(header->e_ident, ELF_MAGIC, 4) != 0) {
        return false;
    }

    /* Verify architecture class is 32-bit (e_ident[4] == 1) */
    if (header->e_ident[4] != 1) {
        return false;
    }

    /* Verify executable type */
    if (header->e_type != ET_EXEC) {
        return false;
    }

    /* Verify machine architecture is x86/386 */
    if (header->e_machine != EM_386) {
        return false;
    }

    /* Verify ELF version */
    if (header->e_version != 1) {
        return false;
    }

    return true;
}
