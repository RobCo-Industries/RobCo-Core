"""Boot examples/umode on every board: user-mode tasks, system calls, faults and kills.

The example runs each case as its own task, first with the MMU off (the
cases that do not need protection), then with the MMU on (every case),
and prints one ev=task marker per task.
"""

import re

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
AUDIT = ["init", "psr", "cpsid", "stubsvc", "killexit", "afterkill"]
NO_MMU = ["return", "exit", "regs", "badptr", "priv", "undef", "rdkern", "rdperiph",
          "spin", "svcspin", "thumb"] + AUDIT
ALL = ["return", "exit", "regs", "badptr", "priv", "undef", "rdkern", "wrkern", "rdperiph",
       "wrro", "jump", "stack", "spin", "svcspin", "thumb"] + AUDIT + ["peek"]
WATCHDOG = ("spin", "svcspin", "cpsid")
# umode_run() errors.
ERR = {"stub": -14, "align": -3, "overlap": -5, "kernel": -9, "periph": -11, "ram": -7}


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
        assert (t["reason"], t["code"]) == ("exit", "7")     # 99: nested run not refused
    elif n in ("regs", "badptr"):
        assert (t["reason"], t["code"]) == ("exit", "0")     # see user/prog.S for codes
    elif n == "priv":
        assert (t["reason"], t["type"]) == ("fault", "undef")
        assert int(t["pc"], 16) == slot(4)
    elif n == "undef":
        assert (t["reason"], t["type"]) == ("fault", "undef")
        assert int(t["pc"], 16) == slot(5)
    elif n == "thumb":
        assert (t["reason"], t["type"]) == ("fault", "undef")
        assert int(t["pc"], 16) == slot(14, 8)              # exact Thumb address
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
    else:
        return False
    return True


def check_mmu(t, board, canary_addr):
    n = t["name"]
    if check_common(t, True):
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


def test_umode(board, umode_vm):
    vm = umode_vm
    canary_addr = int(events(vm, "boot")[0]["canary_addr"], 16)
    tasks = events(vm, "task")
    off = [t for t in tasks if t["mmu"] == "0"]
    on = [t for t in tasks if t["mmu"] == "1"]
    assert [t["name"] for t in off] == NO_MMU, vm.serial
    assert [t["name"] for t in on] == ALL, vm.serial
    assert events(vm, "mmu")[0]["on"] == "1", vm.serial

    for t in off:
        check_no_mmu(t, canary_addr)
    for t in on:
        check_mmu(t, board, canary_addr)

    # Kernel memory never changed, and the tick kept running across tasks.
    assert all(t["canary"] == "ok" for t in tasks), vm.serial
    ticks = [int(t["ticks"]) for t in tasks]
    assert ticks == sorted(ticks), vm.serial
    for t in tasks:
        if t["name"] in WATCHDOG:
            assert int(t["ticks"]) >= 30, vm.serial        # killed after 30 ticks of run time
    done = events(vm, "done")[0]
    assert done["tasks"] == str(len(NO_MMU) + len(ALL)) and done["canary"] == "ok", vm.serial
    assert int(done["ticks"]) > ticks[0] + 6 * 30, vm.serial

    # The bad pointer was refused; the good write printed once per pass.
    writes = events(vm, "write")
    assert [w["text"] for w in writes] == ["hello_from_user_mode"] * 2, vm.serial

    errors = events(vm, "errors")
    assert [e["mmu"] for e in errors] == ["0", "1"], vm.serial
    for e in errors:
        want = dict(ERR, align=ERR["align"] if e["mmu"] == "1" else 0)
        assert {k: int(e[k]) for k in ERR} == want, vm.serial
    # After the refusals, kernel memory is still out of the task's reach.
    after = events(vm, "refused_after")
    assert len(after) == 1, vm.serial
    assert (after[0]["reason"], after[0]["type"]) == ("fault", "data_abort"), vm.serial
    assert int(after[0]["addr"], 16) == canary_addr and after[0]["canary"] == "ok", vm.serial
    stale = events(vm, "stale")[0]
    assert (stale["rc"], stale["reason"], stale["code"]) == ("0", "returned", "42"), vm.serial
    assert not events(vm, "error"), vm.serial
    assert not EXCEPTION.search(vm.serial), vm.serial

    # IRQs still work after every task: UART input is echoed.
    vm.send("umode-echo-7\r")
    vm.wait_for("umode-echo-7\n", timeout=30)


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

