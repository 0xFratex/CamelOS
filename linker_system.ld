/* linker_system.ld */
ENTRY(_start)

SECTIONS
{
    /* Load Kernel at 1MB */
    . = 0x100000;

    .text : ALIGN(4K)
    {
        /* Force the entry point code to be first! */
        *(.text.entry)
        *(.text)
        /* Multiboot header can go anywhere after the entry point for MBR boots */
        /* But keep it near the start for GRUB compatibility if you use ISO */
        KEEP(*(.multiboot))
    }

    .data : ALIGN(4K)
    {
        *(.data)
    }

    .bss : ALIGN(4K)
    {
        _bss_start = .;
        *(.COMMON)
        *(.bss)
        _bss_end = .;
    }
}