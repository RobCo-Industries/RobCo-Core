/*
 * Firmware stub: does what the Raspberry Pi GPU firmware and its armstub
 * do before a 32-bit kernel at 0x8000 runs.
 *
 * Loaded raw at 0x0. All cores start here. Steps:
 *   1. Park secondary cores in a wfi loop (multi-core parts only).
 *   2. Read the parameter block at PARAMS (poked by the harness).
 *   3. Write ATAGS at 0x100: CORE, MEM, NONE.
 *   4. Optionally switch Secure SVC to Non-secure HYP with smc.
 *   5. r0 = 0, r1 = machine id, r2 = 0x100, jump to 0x8000.
 *
 * Layout: 0x000 reset and monitor vectors, 0x100-0x3ff ATAGS area,
 * 0x400 code. PARAMS sits at 0x7f00, outside the loaded file, so the
 * harness can poke it with loader words without overlapping ROMs.
 *
 * Must run on ARMv6 (ARM1176): no movw/movt, no isb/dsb mnemonics.
 */

#define PARAMS        0x7f00   /* magic, machine id, mem size, flags */
#define PARAMS_MAGIC  0x57464352 /* "RCFW" */
#define FLAG_HYP      1
#define ATAGS         0x100
#define KERNEL        0x8000
#define PART_ARM1176  0xb76
#define MODE_HYP      0x1a

    .text
    .arm
    .global _start
_start:
    b       reset

/* Monitor vector table for MVBAR. Only the SMC entry (+0x08) is used. */
    .balign 32
mon_vectors:
    b       .                       @ unused
    b       .                       @ undef
    b       mon_smc                 @ smc
    b       .                       @ prefetch abort
    b       .                       @ data abort
    b       .                       @ unused
    b       .                       @ irq
    b       .                       @ fiq

    .org    0x400
reset:
    /* 1. Detect the CPU. ARM1176 has no MPIDR and no HYP. */
    mrc     p15, 0, r4, c0, c0, 0           @ MIDR
    mov     r3, r4, lsl #16
    mov     r3, r3, lsr #20                 @ part number, bits [15:4]
    ldr     r12, =PART_ARM1176
    cmp     r3, r12
    moveq   r11, #1                         @ r11 = 1 on ARM1176
    beq     params
    mov     r11, #0
    mrc     p15, 0, r3, c0, c0, 5           @ MPIDR
    ands    r3, r3, #3
    bne     park

params:
    /* 2. r5 = machine id, r6 = memory size, r7 = flags. */
    ldr     r12, =PARAMS
    ldr     r3, [r12]
    ldr     r0, =PARAMS_MAGIC
    cmp     r3, r0
    ldreq   r5, [r12, #4]
    ldreq   r6, [r12, #8]
    ldreq   r7, [r12, #12]
    ldrne   r5, =0xc42
    movne   r6, #0x10000000                 @ 256 MiB default
    movne   r7, #0

    /* 3. ATAGS. */
    ldr     r12, =ATAGS
    mov     r0, #5                          @ ATAG_CORE, 5 words
    ldr     r1, =0x54410001
    mov     r2, #1                          @ flags: read-only root
    mov     r3, #0x1000                     @ page size
    mov     r8, #0                          @ root device
    stmia   r12!, {r0-r3, r8}
    mov     r0, #4                          @ ATAG_MEM, 4 words
    ldr     r1, =0x54410002
    mov     r2, r6                          @ size
    mov     r3, #0                          @ start
    stmia   r12!, {r0-r3}
    mov     r0, #0                          @ ATAG_NONE
    mov     r1, #0
    stmia   r12!, {r0-r1}

    /* 4. HYP switch, if asked and possible, and not already in HYP. */
    tst     r7, #FLAG_HYP
    beq     enter
    cmp     r11, #0
    bne     enter
    mrs     r0, cpsr
    and     r0, r0, #0x1f
    cmp     r0, #MODE_HYP
    beq     enter
    ldr     r0, =mon_vectors
    mcr     p15, 0, r0, c12, c0, 1          @ MVBAR
    mov     r0, #0
    mcr     p15, 0, r0, c7, c5, 4           @ ISB
    smc     #0                              @ returns here in HYP

enter:
    /* 5. Kernel entry, as the firmware does it. */
    mov     r0, #0
    mov     r1, r5
    ldr     r2, =ATAGS
    ldr     pc, =KERNEL

park:
    wfi
    b       park

/* Monitor mode: go Non-secure with HYP enabled, return to HYP. */
mon_smc:
    ldr     r0, =0x131                      @ SCR: NS, FW, AW, HCE
    mcr     p15, 0, r0, c1, c1, 0
    mov     r0, #0
    mcr     p15, 0, r0, c7, c5, 4           @ ISB
    ldr     r0, =0x1da                      @ HYP, A/I/F masked
    msr     spsr_cxsf, r0
    movs    pc, lr

    .ltorg
