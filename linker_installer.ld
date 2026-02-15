/* linker_installer.ld - For the ISO Installer (GRUB) */
ENTRY(_start)

SECTIONS
{
    /* GRUB kernels must load at 1MB or higher */
    . = 1M;

    /* The Multiboot header MUST come first for GRUB to find it */
    .text : ALIGN(4K)
    {
        KEEP(*(.multiboot))
        *(.text)
    }

    .rodata : ALIGN(4K)
    {
        *(.rodata)
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