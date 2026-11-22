/* The user test programs, embedded as one raw binary. */
    .section .rodata.user_prog, "a"
    .balign 4
    .global user_prog_start, user_prog_end
user_prog_start:
    .incbin "prog.bin"
user_prog_end:
