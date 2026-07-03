/* Linker script for ArcadeOS kernel
 *
 * The kernel is linked at 1 MiB and converted to a flat binary
 * (objcopy -O binary); the bootloader jumps to the first byte, so
 * .text.start (the _start stub in boot.asm) must come first.
 */

ENTRY(_start)

SECTIONS
{
    . = 1M;

    .text :
    {
        *(.text.start)
        *(.text)
    }

    .rodata :
    {
        *(.rodata)
    }

    .data :
    {
        *(.data)
    }

    /* _start zeroes [__bss_start, kernel_end) with 8-byte stores — flat
     * binaries carry no NOBITS sections, so nobody else will */
    . = ALIGN(8);
    __bss_start = .;

    .bss :
    {
        *(COMMON)
        *(.bss)
    }

    /* Symbol marking the end of the kernel – used by the heap allocator */
    . = ALIGN(8);
    kernel_end = .;
}
