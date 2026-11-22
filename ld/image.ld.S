/*
 * RobCo AArch32 image. Run through cpp with -DLINK_ADDR=... and the
 * project's robco_config.h forced in, so stack sizes can be overridden.
 * ROBCO_SLOT_END, from robco_config.h or SLOT_END in mk/robco.mk, is the
 * first byte past the image's slot in docs/spec/MEMORY_MAP.md. When set,
 * the link fails if the image and its stacks do not fit below it.
 */
#ifndef LINK_ADDR
#error "LINK_ADDR must be defined"
#endif
#ifndef ROBCO_STACK_SVC_SIZE
#define ROBCO_STACK_SVC_SIZE 0x4000
#endif
#ifndef ROBCO_STACK_IRQ_SIZE
#define ROBCO_STACK_IRQ_SIZE 0x1000
#endif
#ifndef ROBCO_STACK_FIQ_SIZE
#define ROBCO_STACK_FIQ_SIZE 0x1000
#endif
#ifndef ROBCO_STACK_ABT_SIZE
#define ROBCO_STACK_ABT_SIZE 0x1000
#endif
#ifndef ROBCO_STACK_UND_SIZE
#define ROBCO_STACK_UND_SIZE 0x1000
#endif

ENTRY(_start)

SECTIONS
{
    . = LINK_ADDR;
    __image_start = .;

    /* Branch to the real entry, padded to 0x20. */
    .boot : {
        KEEP(*(.text.boot.head))
    }

    .rimg LINK_ADDR + 0x20 : {
        KEEP(*(.rimg))
    }

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
        . = ALIGN(4);
    }
    __image_end = .;

    .bss (NOLOAD) : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }

    /* Mode stacks. Not zeroed. */
    .stack (NOLOAD) : ALIGN(16) {
        . += ROBCO_STACK_IRQ_SIZE;
        __stack_irq_top = .;
        . += ROBCO_STACK_FIQ_SIZE;
        __stack_fiq_top = .;
        . += ROBCO_STACK_ABT_SIZE;
        __stack_abt_top = .;
        . += ROBCO_STACK_UND_SIZE;
        __stack_und_top = .;
        . += ROBCO_STACK_SVC_SIZE;
        __stack_svc_top = .;
    }
    __image_mem_end = .;

    /* First free byte after the image. k_heap_init can start here. */
    __heap_start = ALIGN(16);

#ifdef ROBCO_SLOT_END
    __slot_end = ROBCO_SLOT_END;
#else
    __slot_end = 0;
#endif

    __image_file_size = __image_end - __image_start;
    __image_mem_size = __image_mem_end - __image_start;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
    }
}

ASSERT(SIZEOF(.boot) == 0x20, "branch area must be 0x20 bytes")
ASSERT(SIZEOF(.rimg) == 64, "rimg header must be 64 bytes")
#ifdef ROBCO_SLOT_END
ASSERT(__image_mem_end <= ROBCO_SLOT_END, "image and stacks do not fit below ROBCO_SLOT_END, the end of the slot; see docs/spec/MEMORY_MAP.md")
#endif
