"""Boot examples/umode on every board: user-mode tasks, system calls, faults and kills.

The example runs each case as its own task, first with the MMU off (the
cases that do not need protection), then with the MMU on (every case),
and prints one ev=task marker per task.
"""

import os
import re
import subprocess

import pytest

from harness.boot import boot

DONE = re.compile(r"^@RC stage=umode ev=done .*\n", re.M)
EXCEPTION = re.compile(r"^@RC stage=umode ev=exception .*\n", re.M)

CODE = 0x00400000
DATA = 0x01000000
SLOT = 0x40
PERIPH = {"raspi0": 0x20000000, "raspi1ap": 0x20000000,
          "raspi2b": 0x3F000000, "raspi3b": 0x3F000000}
# Short-descriptor fault status, FSR bits [10] and [3:0], section forms.
FS_TRANSLATION = 0x05
FS_PERMISSION = 0x0D
WNR = 1 << 11

# Cases that run with the MMU off too, in order.
AUDIT = ["init", "psr", "cpsid", "stubsvc", "killexit", "afterkill", "killfirst"]
NO_MMU = ["return", "exit", "regs", "badptr", "priv", "undef", "rdkern", "rdperiph",
          "spin", "svcspin", "thumb", "thumb2"] + AUDIT
ALL = ["return", "exit", "regs", "badptr", "priv", "undef", "rdkern", "wrkern", "rdperiph",
       "wrro", "jump", "jumpkern", "stack", "spin", "svcspin", "thumb", "thumb2"] + AUDIT + ["peek"]
# ARM1176 has no Thumb-2: the 32-bit Thumb case runs on the other CPUs only.
NO_THUMB2 = ("raspi0", "raspi1ap")
WATCHDOG = ("spin", "svcspin", "cpsid")
# umode_run() errors per configuration, in order. Alignment is checked only with the MMU on.
ERRORS = {
    "0": ([("stub", -14), ("overlap", -5), ("kernel", -9), ("periph", -11), ("ram", -7)],
          "align"),
    "1": ([("stub", -14), ("align", -3), ("overlap", -5), ("kernel", -9), ("periph", -11),
           ("ram", -7)], "none"),
}


def events(vm, name):
    return [m for m in vm.markers() if m.get("stage") == "umode" and m.get("ev") == name]


def fault_status(v):
    v = int(v, 16)
    return ((v >> 6) & 0x10) | (v & 0xF)


def slot(n, off=0):
    return CODE + n * SLOT + off


def check_common(t, mmu):
    """Outcomes that do not depend on the MMU."""
    n = t["name"]
    if n == "return":
        assert (t["reason"], t["code"]) == ("returned", "42")
    elif n == "exit":
        assert (t["reason"], t["code"]) == ("exit", "7")     # 99: empty nested task not refused
    elif n in ("regs", "badptr"):
        assert (t["reason"], t["code"]) == ("exit", "0")     # see user/prog.S for codes
    elif n == "priv":
        assert (t["reason"], t["type"]) == ("fault", "undef")
        assert int(t["pc"], 16) == slot(4)
    elif n == "undef":
        assert (t["reason"], t["type"]) == ("fault", "undef")
        assert int(t["pc"], 16) == slot(5)
    elif n == "thumb":
        # 16-bit Thumb undefined instruction: pc is its exact address.
        assert (t["reason"], t["type"], t["thumb"]) == ("fault", "undef", "1")
        assert int(t["pc"], 16) == slot(14, 8)
    elif n == "thumb2":
        # 32-bit Thumb-2 undefined instruction: pc is its first halfword, not the second.
        assert (t["reason"], t["type"], t["thumb"]) == ("fault", "undef", "1")
        assert int(t["pc"], 16) == slot(22, 8)
    elif n in ("spin", "svcspin"):
        assert (t["reason"], t["code"]) == ("killed", "1")
        pc = int(t["pc"], 16)
        assert slot(12 if n == "spin" else 13) <= pc < slot(14)
    elif n == "init":
        # First entry: r4-r12 and the flags are 0, no kernel value leaks.
        assert (t["reason"], t["code"]) == ("returned", "0"), t
    elif n == "psr":
        # USR, ARM, IRQs on, FIQ masked, A clear: whatever the handler wrote.
        assert t["reason"] == "exit", t
        assert int(t["code"]) & 0x1FF == 0x50, t
    elif n == "cpsid":
        # The task cannot mask IRQs: the watchdog still kills it.
        assert (t["reason"], t["code"]) == ("killed", "1"), t
        assert slot(18) <= int(t["pc"], 16) < slot(19)
    elif n == "stubsvc":
        # An svc at the stub only ends the task, with its own r0.
        assert (t["reason"], t["code"]) == ("returned", "77"), t
        assert int(t["pc"], 16) == DATA + 4
    elif n == "killexit":
        # A kill requested during the exiting call: the exit wins.
        assert (t["reason"], t["code"]) == ("exit", "5"), t
    elif n == "afterkill":
        # The next task does not inherit that kill request.
        assert (t["reason"], t["code"]) == ("returned", "42"), t
    elif n == "killfirst":
        # The tick's kill request came first: the handler's later one is ignored.
        assert (t["reason"], t["code"]) == ("killed", "2"), t
    else:
        return False
    return True


def check_mmu(t, board, canary_addr):
    n = t["name"]
    if check_common(t, True) or n == "jumpkern":    # check_jumpkern()
        return
    pc, addr = int(t["pc"], 16), int(t["addr"], 16)
    status = int(t["status"], 16)
    want = {
        "rdkern": ("data_abort", slot(6), canary_addr, FS_PERMISSION, False),
        "wrkern": ("data_abort", slot(7), canary_addr, FS_PERMISSION, True),
        "rdperiph": ("data_abort", slot(8), PERIPH[board] + 0x3004, FS_PERMISSION, False),
        "wrro": ("data_abort", slot(9), CODE + 0x800, FS_PERMISSION, True),
        "jump": ("prefetch_abort", 0xE0000000, 0xE0000000, FS_TRANSLATION, None),
        "stack": ("data_abort", slot(11), 0x00FFFFFC, FS_PERMISSION, True),
        # The earlier tasks' data region is privileged again.
        "peek": ("data_abort", slot(6), DATA, FS_PERMISSION, False),
    }[n]
    assert t["reason"] == "fault" and t["type"] == want[0]
    assert (pc, addr) == (want[1], want[2])
    assert fault_status(t["status"]) == want[3]
    if want[4] is not None:
        assert bool(status & WNR) == want[4], "WnR"


def check_no_mmu(t, canary_addr):
    if check_common(t, False):
        return
    # No protection with the MMU off: the reads succeed.
    assert t["reason"] == "returned", t
    if t["name"] == "rdkern":
        assert int(t["code"]) & 0xFFFFFFFF == 0x5AFEC0DE


@pytest.fixture
def umode_vm(board, example_image):
    with boot(example_image("umode"), board) as vm:
        vm.wait_for(DONE, timeout=180)
        yield vm


def test_umode(board, umode_vm, example_image):
    vm = umode_vm
    canary_addr = int(events(vm, "boot")[0]["canary_addr"], 16)
    tasks = events(vm, "task")
    off = [t for t in tasks if t["mmu"] == "0"]
    on = [t for t in tasks if t["mmu"] == "1"]
    skip = ["thumb2"] if board in NO_THUMB2 else []
    no_mmu = [n for n in NO_MMU if n not in skip]
    every = [n for n in ALL if n not in skip]
    assert [t["name"] for t in off] == no_mmu, vm.serial
    assert [t["name"] for t in on] == every, vm.serial
    assert events(vm, "mmu")[0]["on"] == "1", vm.serial

    for t in off:
        check_no_mmu(t, canary_addr)
    for t in on:
        check_mmu(t, board, canary_addr)
    check_jumpkern(jumpkern(vm), board, os.path.splitext(example_image("umode"))[0] + ".elf")

    # Kernel memory never changed, and the tick kept running across tasks.
    assert all(t["canary"] == "ok" for t in tasks), vm.serial
    ticks = [int(t["ticks"]) for t in tasks]
    assert ticks == sorted(ticks), vm.serial
    for t in tasks:
        if t["name"] in WATCHDOG:
            assert int(t["ticks"]) >= 30, vm.serial        # killed after 30 ticks of run time
    done = events(vm, "done")[0]
    assert done["tasks"] == str(len(no_mmu) + len(every)) and done["canary"] == "ok", vm.serial
    assert int(done["ticks"]) > ticks[0] + 6 * 30, vm.serial

    # The bad pointer was refused; the good write printed once per pass.
    writes = events(vm, "write")
    assert [w["text"] for w in writes] == ["hello_from_user_mode"] * 2, vm.serial

    errors = events(vm, "errors")
    assert [e["mmu"] for e in errors] == ["0", "1"], vm.serial
    for e in errors:
        cases, skipped = ERRORS[e["mmu"]]
        # Every case in order. "ran" or "killed" in place of a code: a refused layout started.
        got = [(k, v) for k, v in e.items() if k not in ("stage", "ev", "mmu", "skipped")]
        assert got == [(k, str(v)) for k, v in cases], vm.serial
        assert e["skipped"] == skipped, vm.serial
    # After the refusals, kernel memory is still out of the task's reach.
    after = events(vm, "refused_after")
    assert len(after) == 1, vm.serial
    assert (after[0]["reason"], after[0]["type"]) == ("fault", "data_abort"), vm.serial
    assert int(after[0]["addr"], 16) == canary_addr and after[0]["canary"] == "ok", vm.serial
    stale = events(vm, "stale")[0]
    assert (stale["rc"], stale["reason"], stale["code"]) == ("0", "returned", "42"), vm.serial
    # Task sections left privileged read-only or unmapped by the caller: the task still runs.
    pre = [(p["name"], p["rc"], p["reason"], p["code"]) for p in events(vm, "premapped")]
    assert pre == [("ro", "0", "returned", "42"), ("none", "0", "returned", "42")], vm.serial
    # A section outside the regions left user-accessible: refused, nothing runs. Then restored.
    um = [(u["name"], u["rc"], u["ran"], u["code"]) for u in events(vm, "usermap")]
    assert um == [("ro", "-15", "0", "0"), ("rw", "-15", "0", "0"),
                  ("restored", "0", "1", "42")], vm.serial
    assert not events(vm, "error"), vm.serial
    assert not EXCEPTION.search(vm.serial), vm.serial

    # IRQs still work after every task: UART input is echoed.
    vm.send("umode-echo-7\r")
    vm.wait_for("umode-echo-7\n", timeout=30)


# QEMU 10.1 lets ARMv6 fetch from privileged-only sections in USR mode: get_S1prot()
# in target/arm/ptw.c grants PAGE_EXEC on every pre-v7 CPU. Real ARM1176 must abort.
QEMU_V6_EXEC = ("raspi0", "raspi1ap")
KERNEL = 0x8000
PRIV_OPS = ("mrc", "mcr", "mrrc", "mcrr", "cps", "cpsid", "cpsie", "srs", "rfe")


def first_priv_insn(elf):
    """Address and text of the first instruction undefined in USR mode on the boot path.

    Follows the branch at the image start, then scans for the first coprocessor
    access or mode change. The kernel runs straight through to it from the entry.
    """
    dis = subprocess.run(["arm-none-eabi-objdump", "-d", elf], check=True,
                         capture_output=True, text=True).stdout
    insns = {}
    for m in re.finditer(r"^\s*([0-9a-f]+):\s+[0-9a-f]{8}\s+(\S+)\s*(.*)$", dis, re.M):
        insns[int(m.group(1), 16)] = (m.group(2), m.group(3))
    op, args = insns[KERNEL]
    assert op == "b", (op, args)
    pc = int(args.split()[0], 16)
    while insns[pc][0] not in PRIV_OPS:
        assert insns[pc][0].split(".")[0] not in ("b", "bl", "bx", "blx", "pop", "ldm"), \
            (hex(pc), insns[pc])
        pc += 4
    return pc, insns[pc]


def jumpkern(vm):
    t = [t for t in events(vm, "task") if t["name"] == "jumpkern"]
    assert len(t) == 1, vm.serial
    return t[0]


def check_jumpkern(t, board, elf):
    """Exact jumpkern outcome per CPU. Fails if the other CPU class's outcome shows up."""
    assert t["canary"] == "ok", t
    assert t["reason"] == "fault", t        # not returned, not killed by the watchdog
    if board in QEMU_V6_EXEC:
        assert t["type"] != "prefetch_abort", (
            "QEMU now refuses the USR fetch on ARM1176: move this board to the "
            "prefetch_abort expectation and drop QEMU_V6_EXEC", t)
        # The task runs the boot code in USR mode until the first privileged instruction.
        # A USR fault is reported as a task fault; a kernel fault would be ev=exception.
        want, insn = first_priv_insn(elf)
        assert (t["type"], t["thumb"]) == ("undef", "0"), t
        assert int(t["pc"], 16) == want, (t, hex(want), insn)
    else:
        assert t["type"] == "prefetch_abort", (t, "ARM1176-style fetch of kernel code")
        assert int(t["pc"], 16) == KERNEL and int(t["addr"], 16) == KERNEL, t
        assert fault_status(t["status"]) == FS_PERMISSION, t


def test_umode_jump_kernel(board, umode_vm, example_image):
    """A jump into the kernel image (mapped, privileged-only) from USR mode.

    Cortex-A7/A53: prefetch abort at the target. ARM1176 under QEMU 10.1: the
    fetch is allowed, so the task stops at the boot code's first privileged
    instruction with an undefined-instruction fault.
    """
    elf = os.path.splitext(example_image("umode"))[0] + ".elf"
    check_jumpkern(jumpkern(umode_vm), board, elf)
    assert not EXCEPTION.search(umode_vm.serial), umode_vm.serial


@pytest.mark.parametrize("variant,flag,ev,etype,call", [
    ("kfault", "UMODE_KFAULT", "kfault", "undef", "6"),
    ("ksvc", "UMODE_KSVC", "ksvc", "svc", None),
])
def test_umode_kernel_fault(board, example_image, variant, flag, ev, etype, call):
    """A fault in a handler, or a privileged svc, stays fatal with umode linked."""
    with boot(example_image("umode", variant, f"-D{flag}=1"), board) as vm:
        vm.wait_for(EXCEPTION, timeout=180)
        assert events(vm, ev), vm.serial
        exc = events(vm, "exception")
        assert len(exc) == 1 and exc[0]["type"] == etype, vm.serial
        assert int(exc[0]["spsr"], 16) & 0x1F == 0x13, "taken in SVC mode"
        # A handler fault names the call that was running.
        assert [c["nr"] for c in events(vm, "umode_call")] == ([call] if call else []), vm.serial
        assert not events(vm, "done"), vm.serial


MMU_CHANGED = re.compile(r"^@RC stage=umode ev=umode_mmu_changed .*\n", re.M)


def test_umode_handler_mmu_change(board, example_image):
    """A handler that turns the MMU off mid-task halts the kernel before the task resumes."""
    with boot(example_image("umode", "kmmu", "-DUMODE_KMMU=1"), board) as vm:
        vm.wait_for(MMU_CHANGED, timeout=180)
        assert events(vm, "kmmu"), vm.serial
        assert [c["nr"] for c in events(vm, "umode_mmu_changed")] == ["9"], vm.serial
        assert not events(vm, "done"), vm.serial


# Nested tasks: an outer task whose handler starts an inner task (SYS_RUN).
O_PRIV, SHARED, UNMAPPED = 0x01200000, 0x01300000, 0xE0000000
NEST_OFF = ["value", "exit", "undef", "rdpriv", "rdshared", "spin", "busy", "depth", "range",
            "kill", "badkern", "badstub", "badshare", "seq"]
NEST_ON = ["value", "exit", "undef", "rdpriv", "rdshared", "wrshared", "rdunmap", "pabt", "spin",
           "busy", "depth", "range", "kill", "badkern", "badstub", "badmap", "badshare", "seq"]
# Inner tasks each outer case starts, in order. "busy" starts "ret" at depth 2 first: refused.
INNERS = {"value": ["ret"], "busy": ["ret", "busy"], "range": ["range"], "kill": ["kill"],
          "seq": ["kill", "exit", "ret"]}
# Outer result: (reason, code). Default: returned, the inner code + 1.
OUTER = {"kill": ("killed", 6), "badkern": ("returned", -8), "badstub": ("returned", -13),
         "badmap": ("returned", -14), "badshare": ("returned", -15), "seq": ("returned", 42)}


def u32(v):
    return int(v) & 0xFFFFFFFF


def check_inner(i, mmu):
    """One inner task's outcome, from the handler's ev=inner marker."""
    n = i["name"]
    assert i["canary"] == "ok", i
    if i["depth"] == "2":
        # Started from an inner task's handler: refused, nothing changed.
        assert (n, i["after"], i["rc"], i["reason"]) == ("ret", "2", "-1", "none"), i
        return
    assert (i["depth"], i["after"]) == ("1", "1"), i
    rc = {"badkern": "-9", "badstub": "-14", "badmap": "-15", "badshare": "-16"}.get(n, "0")
    assert i["rc"] == rc, i
    if rc != "0":
        return
    pc, addr = int(i["pc"], 16), int(i["addr"], 16)
    got = (i["reason"], i["type"])
    if n == "ret":
        assert (got, i["code"]) == (("returned", "none"), "41"), i
    elif n == "exit":
        assert (got, i["code"]) == (("exit", "none"), "9"), i
    elif n == "undef":
        assert got == ("fault", "undef") and pc == slot(30), i
    elif n == "rdpriv" and mmu:
        # The outer task's private region is privileged while the inner task runs.
        assert got == ("fault", "data_abort") and (pc, addr) == (slot(31), O_PRIV), i
        assert fault_status(i["status"]) == FS_PERMISSION, i
    elif n == "rdpriv":
        assert got == ("returned", "none") and u32(i["code"]) == 0xC0FFEE01, i
    elif n == "rdshared":
        assert got == ("returned", "none") and u32(i["code"]) == 0x5A5A0001, i
    elif n == "wrshared":
        assert got == ("fault", "data_abort") and (pc, addr) == (slot(32), SHARED), i
        assert fault_status(i["status"]) == FS_PERMISSION and int(i["status"], 16) & WNR, i
    elif n == "rdunmap":
        assert got == ("fault", "data_abort") and (pc, addr) == (slot(31), UNMAPPED), i
        assert fault_status(i["status"]) == FS_TRANSLATION, i
    elif n == "pabt":
        assert got == ("fault", "prefetch_abort") and pc == UNMAPPED, i
    elif n == "spin":
        assert (got, i["code"]) == (("killed", "none"), "1") and slot(34) <= pc < slot(35), i
    elif n in ("busy", "depth"):
        assert (got, i["code"]) == (("returned", "none"), "2"), i
    elif n == "range":
        assert (got, i["code"]) == (("returned", "none"), str(0x1F)), i
    elif n == "kill":
        assert (got, i["code"]) == (("killed", "none"), "5") and slot(38) <= pc < slot(39), i
    else:
        raise AssertionError(i)


def test_umode_nested(board, umode_vm):
    """A system-call handler starts an inner task and gets control back, MMU off and on."""
    vm = umode_vm
    nests = events(vm, "nest")
    inners = events(vm, "inner")
    for mmu, names in (("0", NEST_OFF), ("1", NEST_ON)):
        outer = [t for t in nests if t["mmu"] == mmu]
        assert [t["name"] for t in outer] == names + ["plain"], vm.serial
        for t in outer:
            n = t["name"]
            assert t["canary"] == "ok", t
            if n == "plain":
                # A plain top-level task after the nested ones.
                assert (t["rc"], t["reason"], t["code"]) == ("0", "returned", "42"), t
                continue
            want = OUTER.get(n)
            if want is None:
                code = {"value": 41, "exit": 9, "rdshared": 0x5A5A0001, "busy": 2, "depth": 2,
                        "range": 0x0F1F - 1, "spin": 1}.get(n, 0)
                if n == "rdpriv" and mmu == "0":
                    code = 0xC0FFEE01
                want = ("returned", code + 1)
            # A reason "exit" with code 2xx: the call changed a register or a mapping.
            assert (t["reason"], u32(t["code"])) == (want[0], want[1] & 0xFFFFFFFF), t
    # The inner tasks, in order, both passes.
    want = []
    for names in (NEST_OFF, NEST_ON):
        for n in names:
            want += INNERS.get(n, [n])
    assert [i["name"] for i in inners] == want, vm.serial
    split = sum(len(INNERS.get(n, [n])) for n in NEST_OFF)
    for k, i in enumerate(inners):
        check_inner(i, k >= split)
    # umode_depth(): 0 in main before each outer task, 2 in the handler of an inner task.
    depth = events(vm, "depth")
    main = [d["depth"] for d in depth if d["where"] == "main"]
    assert main == ["0"] * (len(NEST_OFF) + len(NEST_ON)), vm.serial
    assert [d["depth"] for d in depth if d["where"] == "handler"] == ["2"] * 4, vm.serial
    assert not events(vm, "error"), vm.serial
    assert not EXCEPTION.search(vm.serial), vm.serial
