/* Raspberry Pi AArch32 kernel image, loaded by the firmware at 0x8000. */
ENTRY(_start)

SECTIONS
{
    . = 0x8000;

    .text : {
        KEEP(*(.text.boot))
        KEEP(*(.text.vectors))
        *(.text .text.*)
    }

    .rodata : ALIGN(4) {
        *(.rodata .rodata.*)
    }

    .ARM.exidx : ALIGN(4) {
        *(.ARM.exidx*)
    }

    .data : ALIGN(4) {
        *(.data .data.*)
    }

    .bss (NOLOAD) : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }

    /* Mode stacks. Not zeroed. */
    .stack (NOLOAD) : ALIGN(16) {
        . += 0x400;
        __stack_irq_top = .;
        . += 0x400;
        __stack_abt_top = .;
        . += 0x400;
        __stack_und_top = .;
        . += 0x4000;
        __stack_svc_top = .;
    }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
    }
}
