/* The DM-32 update program (updater.c) at the fixed address of the RAM the firmware's loader gives it: the update
   area, kept at 0x44400 by src/ckcpu.ld (__update_area_start). One block, the entry first, uploaded as
   is; .bss after it. */
UPDATER_AREA = 0x00044400;
ENTRY(updater_main)
SECTIONS
{
    .image UPDATER_AREA : {
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
