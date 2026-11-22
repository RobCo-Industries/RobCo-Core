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
#define SYS_RUN     11
#define SYS_DEPTH   12
#define SYS_RANGE   13
#define SYS_ARMKILL 14

/* Memory of the nested cases, see examples/umode/main.c. */
#define O_DATA      0x01000000      /* outer task: stub, stack, scratch */
#define O_PRIV      0x01200000      /* outer task only */
#define SHARED      0x01300000      /* outer read-write, inner read-only */
#define I_DATA      0x01400000      /* inner task: stub and stack */
#define O_SAVE      (O_DATA + 0x20) /* outer: real lr, then sp */
#define O_CANARY    0xc0ffee01
#define S_WORD      0x5a5a0001

#define REG_LOOPS   3000

    .section .text, "ax"
    .arm
    .global _ustart
_ustart:

/* 0: return 42. */
    .org    0x000
    mov     r0, #42
    bx      lr

/* 1: a nested umode_run() of an empty task must fail with -2; then exit with code 7 (99 if not). */
    .org    0x040
    mov     r7, #SYS_NEST
    svc     #0
    mvn     r1, #1
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

/* 10: jump to the address in r0 (unmapped, or the kernel image). */
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

/*
 * 24-27: outer tasks of the nested cases. Each starts the inner task
 * whose selector is in r0 through SYS_RUN (r0 = its result, r1 = its
 * end reason, or r0 = the umode_run() error and r1 = 0).
 */

/* 24: run inner r0, check that the call kept this task intact, return the result + 1. */
    .org    0x600
    b       nest_outer

/* 25: run inner r0, then ask for a kill of this task and spin. */
    .org    0x640
    b       nest_kill

/* 26: run inner r0, then check pointers from this task. Returns inner mask | own mask << 8. */
    .org    0x680
    b       nest_range

/* 27: run inners r0, r1 and r2 in a row, return the last result + 1. */
    .org    0x6c0
    b       nest_seq

/* 28-38: inner tasks. A faulting instruction is the first of its slot. */

/* 28: return 41. */
    .org    0x700
    mov     r0, #41
    bx      lr

/* 29: exit with 9. */
    .org    0x740
    mov     r0, #9
    mov     r7, #SYS_EXIT
    svc     #0
    b       .

/* 30: undefined instruction. */
    .org    0x780
    .word   0xe7f000f0
    bx      lr

/* 31: read the word at r0. */
    .org    0x7c0
    ldr     r0, [r0]
    bx      lr

/* 32: write r1 to r0. */
    .org    0x800
    str     r1, [r0]
    bx      lr

/* 33: jump to r0. */
    .org    0x840
    bx      r0

/* 34: spin until the watchdog kills it. */
    .org    0x880
    b       .

/* 35: start another task (must fail with -1), then return the depth, or 99. */
    .org    0x8c0
    mov     r0, #0
    mov     r7, #SYS_RUN
    svc     #0
    mvn     r1, #0
    cmp     r0, r1
    movne   r0, #99
    bxne    lr
    mov     r7, #SYS_DEPTH
    svc     #0
    bx      lr

/* 36: return the depth the handler sees. */
    .org    0x900
    mov     r7, #SYS_DEPTH
    svc     #0
    bx      lr

/* 37: pointer checks from the inner task: returns 0x1f when all hold. */
    .org    0x940
    b       inner_range

/* 38: ask for a kill of this task with reason 5, then spin. */
    .org    0x980
    mov     r0, #5
    mov     r7, #SYS_ARMKILL
    svc     #0
    b       .

    .org    0xa00

/* Fails the outer task with SYS_EXIT and `code` when reg != val. */
    .macro  ochk reg, val, code
    ldr     r0, =\val
    cmp     \reg, r0
    movne   r0, #\code
    bne     nest_fail
    .endm

/* Writes a word to addr, reads it back; exit `code` if it differs. */
    .macro  orw addr, code
    ldr     r1, =\addr
    ldr     r0, =0x600d0000 + \code
    str     r0, [r1]
    ldr     r2, [r1]
    cmp     r2, r0
    movne   r0, #\code
    bne     nest_fail
    .endm

/* r0 = SYS_RANGE(addr, len, write); sets `bit` in r4 if (r0 != 0) == want. */
    .macro  rng addr, len, write, want, bit
    ldr     r0, =\addr
    mov     r1, #\len
    mov     r2, #\write
    mov     r7, #SYS_RANGE
    svc     #0
    cmp     r0, #0
    .if \want
    orrne   r4, r4, #\bit
    .else
    orreq   r4, r4, #\bit
    .endif
    .endm

nest_outer:
    ldr     r2, =O_SAVE
    str     lr, [r2]
    str     sp, [r2, #4]
    ldr     r2, =O_PRIV
    ldr     r1, =O_CANARY
    str     r1, [r2]                        @ the inner task must not reach it
    ldr     r2, =SHARED
    ldr     r1, =S_WORD
    str     r1, [r2]                        @ the inner task may read it
    ldr     r4, =0x44440004                 @ callee-saved values the call must keep
    ldr     r5, =0x55550005
    ldr     r6, =0x66660006
    ldr     r8, =0x88880008
    ldr     r9, =0x99990009
    ldr     r10, =0xaaaa000a
    ldr     r11, =0xbbbb000b
    ldr     lr, =0xeeee000e
    mov     r7, #SYS_RUN
    svc     #0
    mov     r12, r0
    ochk    r4, 0x44440004, 204
    ochk    r5, 0x55550005, 205
    ochk    r6, 0x66660006, 206
    ochk    r8, 0x88880008, 208
    ochk    r9, 0x99990009, 209
    ochk    r10, 0xaaaa000a, 210
    ochk    r11, 0xbbbb000b, 211
    ochk    lr, 0xeeee000e, 214
    ldr     r0, =O_SAVE
    ldr     r0, [r0, #4]
    cmp     sp, r0
    movne   r0, #213
    bne     nest_fail
    ldr     r1, =O_PRIV
    ldr     r1, [r1]
    ochk    r1, O_CANARY, 220
    orw     O_DATA + 0x30, 221              @ every own region is readable and writable again
    orw     O_PRIV + 4, 222
    orw     SHARED + 4, 223
    ldr     r1, =0x00400000
    ldr     r1, [r1]                        @ own code still readable
    ldr     r0, =O_SAVE
    ldr     lr, [r0]
    add     r0, r12, #1
    bx      lr

nest_fail:
    mov     r7, #SYS_EXIT
    svc     #0
    b       .

nest_kill:
    mov     r7, #SYS_RUN
    svc     #0                              @ the inner task is killed with 5
    mov     r0, #6
    mov     r7, #SYS_ARMKILL
    svc     #0
    b       .                               @ killed with 6

nest_range:
    mov     r11, lr
    mov     r7, #SYS_RUN
    svc     #0
    mov     r5, r0                          @ the inner task's mask
    mov     r4, #0
    rng     O_PRIV, 4, 1, 1, 1              @ own memory again
    rng     O_DATA, 4, 1, 1, 2
    rng     I_DATA, 4, 0, 0, 4              @ the inner task's memory: not ours
    rng     SHARED, 4, 1, 1, 8
    orr     r0, r5, r4, lsl #8
    bx      r11

nest_seq:
    mov     r11, lr
    mov     r5, r1
    mov     r6, r2
    mov     r7, #SYS_RUN
    svc     #0
    mov     r0, r5
    svc     #0
    mov     r0, r6
    svc     #0
    add     r0, r0, #1
    bx      r11

inner_range:
    mov     r4, #0
    rng     I_DATA, 16, 1, 1, 1             @ own memory
    rng     O_PRIV, 4, 0, 0, 2              @ outer-only memory
    rng     O_DATA, 4, 0, 0, 4
    rng     SHARED, 4, 0, 1, 8              @ shared read-only
    rng     SHARED, 4, 1, 0, 16
    mov     r0, r4
    bx      lr
    .ltorg

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
