/* The DM-32 flash reader (dumper.c) where the HR_C7000's internal bootloader takes an uploaded program, as the stub of
   Roger VK3KYY's C7000_read_*.py: 0x10000. One block, the entry first; .bss after it, the stack below 0x50000. */
DUMPER_AREA = 0x00010000;
ENTRY(dumper_entry)
SECTIONS
{
    .image DUMPER_AREA : {
        KEEP(*(.text.entry))
        *(.text*)
        *(.rodata*)
        *(.data*)
        . = ALIGN(4);
    }
    .bss (NOLOAD) : {
        *(.bss* .sbss* COMMON)
    }
    /DISCARD/ : { *(.comment) *(.note*) *(.csky.attributes) }
}
