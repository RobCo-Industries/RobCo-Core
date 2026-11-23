/*
 * Universal AArch32 entry for Raspberry Pi Zero/1/2/3 (and QEMU raspi*).
 * Built for ARMv6 (ARM1176JZF-S); runs unchanged on Cortex-A7/A53.
 *
 * Registers kept across the boot path:
 *   r4-r7 entry r0-r3, r8 entry mode, r9 MIDR.
 */

#include <robco/rimg.h>

#if ROBCO_IMG_FLAGS & RIMG_FLAG_META
#error "start.S emits no META block: clear RIMG_FLAG_META in ROBCO_IMG_FLAGS"
#endif

#define MODE_MASK    0x1f
#define MODE_FIQ     0x11
#define MODE_IRQ     0x12
#define MODE_SVC     0x13
#define MODE_ABT     0x17
#define MODE_HYP     0x1a
#define MODE_UND     0x1b
#define PSR_I        0x80
#define PSR_F        0x40
#define PART_ARM1176 0xb76

/* Offset 0x00: branch to the entry point, padded to 0x20. */
    .section .text.boot.head, "ax"
    .arm
    .global _start
_start:
    b       robco_entry
    .balign 32, 0

/* Offset 0x20: image header, see include/robco/rimg.h. No META block. */
    .section .rimg, "a"
    .global robco_rimg
robco_rimg:
    .ascii  ROBCO_IMG_MAGIC
    .org    robco_rimg + 8, 0
    .hword  RIMG_HDR_VERSION
    .hword  RIMG_HDR_SIZE
    .word   ROBCO_IMG_FLAGS
    .word   __image_start
    .word   __image_file_size
    .word   __image_mem_size
    .word   0                               @ crc32, sealed by a host tool
    .word   ROBCO_IMG_ABI_VERSION
    .word   ROBCO_IMG_VERSION
    .word   ROBCO_IMG_SUBTYPE
1:
    .ascii  ROBCO_IMG_NAME
    .org    1b + 16, 0
    .word   0                               @ reserved
    .org    robco_rimg + RIMG_HDR_SIZE

    .section .text.boot, "ax"
    .arm
robco_entry:
    /* 1. Save entry registers and mode. */
    mov     r4, r0
    mov     r5, r1
    mov     r6, r2
    mov     r7, r3
    mrs     r8, cpsr
    and     r8, r8, #MODE_MASK

    /* 2. Park secondary cores, but only on multi-core parts. */
    mrc     p15, 0, r9, c0, c0, 0           @ MIDR
    mov     r10, r9, lsl #16
    mov     r10, r10, lsr #20               @ part number, bits [15:4]
    ldr     r11, =PART_ARM1176
    cmp     r10, r11
    beq     1f
    /* On ARM1176 this encoding reads MIDR again, not MPIDR. */
    mrc     p15, 0, r11, c0, c0, 5          @ MPIDR
    ands    r11, r11, #3
    bne     robco_park
1:
    /* 3. Leave HYP mode if the firmware entered there. */
    cmp     r8, #MODE_HYP
    bne     2f
    mrs     r11, cpsr
    bic     r11, r11, #MODE_MASK
    orr     r11, r11, #(MODE_SVC | PSR_I | PSR_F)
    /* Only reached on ARMv7+; assemble these with the A7 feature set. */
    .cpu    cortex-a7
    .arch_extension virt
    /*
     * HYP trap registers reset to UNKNOWN values. Clear HCR, HCPTR and
     * HSTR so SVC code does not trap to HYP. A set HCPTR.TCP10/TCP11
     * would make the VFP enable below fault.
     */
    mov     r12, #0
    mcr     p15, 4, r12, c1, c1, 0          @ HCR
    mcr     p15, 4, r12, c1, c1, 2          @ HCPTR
    mcr     p15, 4, r12, c1, c1, 3          @ HSTR
    msr     SPSR_hyp, r11
    adr     r11, 2f
    msr     ELR_hyp, r11
    eret
    .cpu    arm1176jzf-s
2:
    /* Force SVC with IRQ/FIQ masked, whatever the entry mode was. */
    cpsid   if, #MODE_SVC

    /*
     * On ARMv7+ the CP15 barrier forms used by mmio.h need SCTLR.CP15BEN.
     * Its reset value is not guaranteed on ARMv8 AArch32, so set it.
     */
    ldr     r11, =PART_ARM1176
    cmp     r10, r11
    beq     3f
    mrc     p15, 0, r11, c1, c0, 0
    orr     r11, r11, #(1 << 5)
    mcr     p15, 0, r11, c1, c0, 0
3:
    /* 4. Mode stacks, inside the image's .stack area. */
    cps     #MODE_IRQ
    ldr     sp, =__stack_irq_top
    cps     #MODE_FIQ
    ldr     sp, =__stack_fiq_top
    cps     #MODE_ABT
    ldr     sp, =__stack_abt_top
    cps     #MODE_UND
    ldr     sp, =__stack_und_top
    cps     #MODE_SVC
    ldr     sp, =__stack_svc_top

    /* 5. Vector base: clear SCTLR.V (low vectors), then VBAR. */
    mrc     p15, 0, r11, c1, c0, 0
    bic     r11, r11, #(1 << 13)
    mcr     p15, 0, r11, c1, c0, 0
    ldr     r11, =robco_vectors
    mcr     p15, 0, r11, c12, c0, 0

    /*
     * 6. VFP access: CPACR cp10/cp11 full access, then FPEXC.EN.
     * The library is soft-float, but projects may use VFP code.
     */
    mrc     p15, 0, r11, c1, c0, 2
    orr     r11, r11, #(0xf << 20)
    mcr     p15, 0, r11, c1, c0, 2
    mov     r11, #0
    mcr     p15, 0, r11, c7, c5, 4          @ prefetch flush (ISB on v6)
    .fpu    vfp
    mov     r11, #(1 << 30)
    vmsr    fpexc, r11
    .fpu    softvfp

    /* 7. Zero BSS. */
    ldr     r0, =__bss_start
    ldr     r1, =__bss_end
    mov     r2, #0
4:
    cmp     r0, r1
    strlo   r2, [r0], #4
    blo     4b

    /* After the clear: robco_boot_args_store lives in BSS. */
    ldr     r0, =robco_boot_args_store
    stmia   r0, {r4-r8}

    /* 8. C entry. Halt if the project returns. */
    bl      robco_start
robco_park:
    wfi
    b       robco_park

    .ltorg

    /*
     * In .bss, outside the file bytes the image CRC covers, so an image can
     * check its own CRC after start. Step 7 zeroes BSS first, then stores
     * the entry registers here, so the clear never erases them.
     */
    .section .bss.robco_boot_args, "aw", %nobits
    .balign 4
    .global robco_boot_args_store
robco_boot_args_store:
    .space  20
