"""AArch64 shim for raspi3b, loaded at 0x80000.

raspi3b starts every core in AArch64 EL3. These words drop a core to
AArch32 at ENTRY (the firmware stub at 0x0), in HYP or SVC mode. The
harness starts all four cores here. The stub parks cores 1 to 3.
"""

ADDR = 0x80000
ENTRY = 0x0
SECONDARY_CPUS = (1, 2, 3)

WORDS = [
    0xD2802620,  # mov  x0, #0x131     SCR_EL3: NS, RES1 bits 4-5, HCE; RW=0 so EL2 is AArch32
    0xD51E1100,  # msr  scr_el3, x0
    0xD2803B40,  # mov  x0, #0x1da     SPSR_EL3: AArch32 HYP, A/I/F masked
    0xD51E4000,  # msr  spsr_el3, x0
    0xD2800000,  # mov  x0, #ENTRY     return address: the firmware stub
    0xD51E4020,  # msr  elr_el3, x0
    0xD69F03E0,  # eret
]

SVC_SPSR = 0xD2803A60  # mov x0, #0x1d3: AArch32 SVC, A/I/F masked (replaces word 2)


def words(mode="hyp"):
    w = list(WORDS)
    if mode == "svc":
        w[2] = SVC_SPSR
    return w
