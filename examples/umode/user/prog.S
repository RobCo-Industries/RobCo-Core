/*
 * Test programs for examples/umode, plain ARMv6 assembly, linked at
 * 0x00400000 (the Pip-Boy ROM slot) and run in USR mode. One case per
 * 0x40-byte slot: the kernel enters at 0x00400000 + case * 0x40 with
 * r0..r3 set per case and lr at the return stub. Where a case faults,
 * the faulting instruction is the first of its slot, so its address is
 * known. System calls: `svc #0`, number in r7, arguments in r0..r3,
 * result in r0.
 */

#define SYS_WRITE   1
#define SYS_TICKS   2
#define SYS_SLEEP   3
#define SYS_EXIT    4
#define SYS_NEST    5
#define SYS_KFAULT  6
#define SYS_BADPSR  7
#define SYS_KILLEXIT 8
#define SYS_KMMU    9
#define SYS_KILLFIRST 10

#define REG_LOOPS   3000

    .section .text, "ax"
    .arm
    .global _ustart
_ustart:

/* 0: return 42. */
    .org    0x000
    mov     r0, #42
    bx      lr

/* 1: a nested umode_run() must fail with -1; then exit with code 7 (99 if it did not). */
    .org    0x040
    mov     r7, #SYS_NEST
    svc     #0
    mvn     r1, #0
    cmp     r0, r1
    moveq   r0, #7
    movne   r0, #99
    mov     r7, #SYS_EXIT
    svc     #0
    b       .

/* 2: register and flag preservation across many system calls. */
    .org    0x080
    b       regs

/* 3: bad pointer to the write service, then a good one. */
    .org    0x0c0
    b       badptr

/* 4: privileged instruction: SCTLR read traps as undefined in USR. */
    .org    0x100
    mrc     p15, 0, r0, c1, c0, 0
    bx      lr

/* 5: undefined instruction. */
    .org    0x140
    .word   0xe7f000f0                      @ udf #0
    bx      lr

/* 6: read kernel memory at r0. */
    .org    0x180
    ldr     r0, [r0]
    bx      lr

/* 7: write r1 to kernel memory at r0. */
    .org    0x1c0
    str     r1, [r0]
    bx      lr

/* 8: read a peripheral register at r0. */
    .org    0x200
    ldr     r0, [r0]
    bx      lr

/* 9: write own read-only region at r0. */
    .org    0x240
    str     r1, [r0]
    bx      lr

/* 10: jump to the unmapped address in r0. */
    .org    0x280
    bx      r0

/* 11: stack overflow: push until the stack leaves its megabyte. */
    .org    0x2c0
    str     r0, [sp, #-4]!
    b       0x2c0 + _ustart

/* 12: infinite loop. */
    .org    0x300
    b       .

/* 13: infinite loop of system calls. */
    .org    0x340
    mov     r7, #SYS_TICKS
1:
    svc     #0
    b       1b

/* 14: Thumb state, then an undefined Thumb instruction at slot + 8. */
    .org    0x380
    adr     r0, 1f + 1
    bx      r0
    .thumb
1:
    .hword  0xde00                          @ Thumb udf #0
    .hword  0x4770                          @ bx lr
    .arm

/* 15: asks the kernel to fault inside its handler (kfault build only). */
    .org    0x3c0
    mov     r7, #SYS_KFAULT
    svc     #0
    bx      lr

/* 16: first entry: r4-r12 and the flags must be 0. Returns their OR. */
    .org    0x400
    orr     r4, r4, r5
    orr     r4, r4, r6
    orr     r4, r4, r7
    orr     r4, r4, r8
    orr     r4, r4, r9
    orr     r4, r4, r10
    orr     r4, r4, r11
    orr     r4, r4, r12
    mrs     r0, cpsr
    and     r0, r0, #0xf8000000
    orr     r0, r0, r4
    bx      lr

/*
 * 17: try to mask IRQs, unmask FIQ and enter SYS mode (all ignored in
 * USR), then a call whose handler writes 0xffffffff to the frame cpsr.
 * Exits with the cpsr after it, or 1 if the first attempt changed it.
 */
    .org    0x440
    cpsid   i
    cpsie   f
    msr     cpsr_c, #0xdf
    mrs     r4, cpsr
    and     r4, r4, #0xff
    cmp     r4, #0x50
    movne   r0, #1
    bne     1f
    mov     r7, #SYS_BADPSR
    svc     #0
    mrs     r0, cpsr
1:
    mov     r7, #SYS_EXIT
    svc     #0
    b       .

/* 18: mask IRQs as far as USR mode allows, then spin: the watchdog must still kill it. */
    .org    0x480
    cpsid   if
    msr     cpsr_c, #0xd0
    b       .

/* 19: svc from the stub with a chosen r0 and a real call number: only ends the task. */
    .org    0x4c0
    mov     r0, #77
    mov     r7, #SYS_NEST
    bx      lr

/* 20: a call during which a kill is requested, then exits from the same call with 5. */
    .org    0x500
    mov     r7, #SYS_KILLEXIT
    svc     #0
    b       .

/* 21: asks the kernel to turn the MMU off inside its handler (kmmu build only). */
    .org    0x540
    mov     r7, #SYS_KMMU
    svc     #0
    bx      lr

/*
 * 22: Thumb state, then a 32-bit Thumb-2 undefined instruction at slot + 8
 * (ARMv7 and later only: ARM1176 has no Thumb-2).
 */
    .org    0x580
    adr     r0, 1f + 1
    bx      r0
    .thumb
1:
    .hword  0xf7f0, 0xa000                  @ udf.w #0
    .hword  0x4770                          @ bx lr
    .arm

/* 23: a kill from the tick, then a second one from the same call: the first wins. */
    .org    0x5c0
    mov     r7, #SYS_KILLFIRST
    svc     #0
    b       .

    .org    0x600

/*
 * regs: r0 = a scratch word in the data region. Before each call, load
 * r1-r6, r8-r12 and lr with values derived from the loop counter, r7
 * with the call number, and N Z C V Q from the counter. After the call,
 * check every register but r0, and the flags. Every 128th call sleeps
 * one tick, so IRQs also land inside a handler. A delay loop between
 * calls lets IRQs land in USR mode. Exit code 0, or 100 + the register
 * that changed (113 sp, 114 lr, 116 flags).
 */
    .macro  check reg, off, code
    ldr     r0, [sp, #4]
    add     r0, r0, #\off
    cmp     \reg, r0
    movne   r0, #\code
    bne     regs_fail
    .endm

regs:
    sub     sp, sp, #16                     @ [sp] flags, [sp+4] counter, [sp+8] scratch address
    str     sp, [r0]                        @ the sp every call must preserve
    str     r0, [sp, #8]
    mov     r0, #0
    str     r0, [sp, #4]
regs_loop:
    ldr     r12, [sp, #4]
    tst     r12, #127
    moveq   r7, #SYS_SLEEP
    movne   r7, #SYS_TICKS
    add     r1, r12, #0x01000000
    add     r2, r12, #0x02000000
    add     r3, r12, #0x03000000
    add     r4, r12, #0x04000000
    add     r5, r12, #0x05000000
    add     r6, r12, #0x06000000
    add     r8, r12, #0x08000000
    add     r9, r12, #0x09000000
    add     r10, r12, #0x0a000000
    add     r11, r12, #0x0b000000
    add     lr, r12, #0x0e000000
    mov     r0, r12, lsl #27
    msr     cpsr_f, r0                      @ N Z C V Q = low 5 bits of the counter
    add     r12, r12, #0x0c000000
    mov     r0, #1                          @ sleep 1 tick (ignored by ticks)
    svc     #0
    mrs     r0, cpsr                        @ before anything changes the flags
    str     r0, [sp]
    ldr     r0, [sp, #8]
    ldr     r0, [r0]
    cmp     sp, r0
    movne   r0, #113
    bne     regs_fail
    check   r1, 0x01000000, 101
    check   r2, 0x02000000, 102
    check   r3, 0x03000000, 103
    check   r4, 0x04000000, 104
    check   r5, 0x05000000, 105
    check   r6, 0x06000000, 106
    check   r8, 0x08000000, 108
    check   r9, 0x09000000, 109
    check   r10, 0x0a000000, 110
    check   r11, 0x0b000000, 111
    check   r12, 0x0c000000, 112
    check   lr, 0x0e000000, 114
    ldr     r0, [sp, #4]
    tst     r0, #127
    moveq   r0, #SYS_SLEEP
    movne   r0, #SYS_TICKS
    cmp     r7, r0
    movne   r0, #107
    bne     regs_fail
    ldr     r7, [sp]
    and     r7, r7, #0xf8000000
    ldr     r0, [sp, #4]
    mov     r0, r0, lsl #27
    cmp     r7, r0
    movne   r0, #116
    bne     regs_fail
    mov     r0, #0x400
1:
    subs    r0, r0, #1
    bne     1b
    ldr     r0, [sp, #4]
    add     r0, r0, #1
    str     r0, [sp, #4]
    ldr     r1, =REG_LOOPS
    cmp     r0, r1
    blo     regs_loop
    mov     r0, #0
regs_fail:
    mov     r7, #SYS_EXIT
    svc     #0
    b       .

/*
 * badptr: r0 = an address outside the task. write(r0, 4) must return
 * -14, then a real write must return its length. Exit 0 if both did.
 */
badptr:
    mov     r1, #4
    mov     r7, #SYS_WRITE
    svc     #0
    mov     r5, r0
    adr     r0, msg
    mov     r1, #(msg_end - msg)
    svc     #0
    mov     r6, r0
    mvn     r0, #13                         @ -14
    cmp     r5, r0
    movne   r0, #1
    bne     1f
    cmp     r6, #(msg_end - msg)
    movne   r0, #2
    moveq   r0, #0
1:
    mov     r7, #SYS_EXIT
    svc     #0
    b       .
    .ltorg

msg:
    .ascii  "hello from user mode"
msg_end:
    .balign 4
