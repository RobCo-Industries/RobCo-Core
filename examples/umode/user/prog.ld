/* Test programs: one flat binary at the ROM slot, 0x00400000. */
ENTRY(_ustart)
SECTIONS
{
    . = 0x00400000;
    .text : { *(.text*) *(.rodata*) }
    /DISCARD/ : { *(.data*) *(.bss*) *(.ARM.*) }
}
