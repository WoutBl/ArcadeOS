/* Linker script for ArcadeOS kernel */

ENTRY(_start)

SECTIONS
{
    . = 1M;

    .boot :
    {
        *(.multiboot)
    }

    .text :
    {
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

    .bss :
    {
        *(.bss)
    }

    /* Symbol marking the end of the kernel – used by the heap allocator */
    kernel_end = .;
}
