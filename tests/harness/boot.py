"""Boot a bare-metal Raspberry Pi image the way the GPU firmware does.

Stdlib only. Usage:

    with boot("build/kernel.img", "raspi2b") as vm:
        vm.wait_for("ev=hello", timeout=5)
        vm.send("abc")
        print(vm.markers())

The image is loaded raw at 0x8000. A firmware stub (tests/fwstub) at 0x0
parks secondary cores, writes ATAGS at 0x100, optionally switches to HYP
and enters the image with r0 = 0, r1 = machine id, r2 = 0x100. On raspi3b
an AArch64 shim at 0x80000 first drops CPU0 to AArch32.
"""

import collections
import contextlib
import os
import re
import socket
import subprocess
import sys
import threading
import time

from . import dump
from .qemu import Qemu, QemuError, loader_words

HERE = os.path.dirname(os.path.abspath(__file__))
FWSTUB_DIR = os.path.join(HERE, "..", "fwstub")
sys.path.insert(0, FWSTUB_DIR)
import a64_shim  # noqa: E402

KERNEL_ADDR = 0x8000
PARAMS_ADDR = 0x7F00     # must match PARAMS in fwstub.S
PARAMS_MAGIC = 0x57464352
FLAG_HYP = 1


class Board:
    def __init__(self, binary, mem, mach_id, auto_mode, a64=False):
        self.binary = binary
        self.mem = mem
        self.mach_id = mach_id
        self.auto_mode = auto_mode
        self.a64 = a64


BOARDS = {
    "raspi0": Board("qemu-system-arm", 512 << 20, 0xC42, "svc"),
    "raspi1ap": Board("qemu-system-arm", 512 << 20, 0xC42, "svc"),
    "raspi2b": Board("qemu-system-arm", 1024 << 20, 0xC43, "hyp"),
    "raspi3b": Board("qemu-system-aarch64", 1024 << 20, 0xC43, "hyp", a64=True),
}

SCRIPT_PORT = 1
SCRIPT_DATA = "| "

# The answer to one script line: its status line ("ok ...", "err ...")
# and the lines a project handler wrote before it, without "| ".
ScriptReply = collections.namedtuple("ScriptReply", "status lines")


def is_script_status(line):
    return line == "ok" or line.startswith(("ok ", "err ")) or line == "err"


MARKER_RE = re.compile(r"@RC((?: +[A-Za-z0-9_]+=\S*)+)")

_stub_lock = threading.Lock()
_stub_path = None


def fwstub_path():
    """Build the firmware stub once per process and return its path."""
    global _stub_path
    with _stub_lock:
        if _stub_path is None:
            subprocess.run(["make", "-s", "-C", FWSTUB_DIR], check=True,
                           stdout=subprocess.DEVNULL)
            _stub_path = os.path.join(FWSTUB_DIR, "build", "fwstub.bin")
        return _stub_path


def complete_lines(text):
    """Return `text` up to its last newline.

    The guest may still be printing the last line, so a tail without a
    newline is dropped. A tail that ends in a bare CR is dropped too.
    """
    return text[:text.rfind("\n") + 1]


def parse_markers(text):
    """Parse complete `@RC stage=<s> ev=<e> k=v ...` lines into dicts.

    A last line without a newline is still arriving and is ignored.
    """
    out = []
    for line in complete_lines(text).replace("\r", "").splitlines():
        m = MARKER_RE.search(line)
        if m:
            out.append(dict(tok.split("=", 1) for tok in m.group(1).split()))
    return out


class Vm:
    """A running guest. Created by boot(); do not build it directly."""

    # Time budget for the whole snapshot, and per QMP read.
    DUMP_BUDGET = 8.0
    DUMP_READ_TIMEOUT = 2.0

    def __init__(self, qemu, serial_socks, elf=None):
        self.qemu = qemu
        self.elf = elf
        self._socks = serial_socks
        self._bufs = [bytearray() for _ in serial_socks]
        self._cond = threading.Condition()
        self._threads = [threading.Thread(target=self._reader, args=(i,), daemon=True)
                         for i in range(len(serial_socks))]
        for t in self._threads:
            t.start()

    def _reader(self, i):
        sock = self._socks[i]
        while True:
            try:
                data = sock.recv(4096)
            except OSError:
                data = b""
            with self._cond:
                if not data:
                    self._cond.notify_all()
                    return
                self._bufs[i] += data
                self._cond.notify_all()

    @property
    def qmp(self):
        """The Qemu object: execute(), hmp(), registers(), wait_event()."""
        return self.qemu

    def _text(self, i):
        with self._cond:
            return self._bufs[i].decode("utf-8", "replace").replace("\r\n", "\n")

    @property
    def serial(self):
        """Text received so far on the PL011, with CRLF turned into LF."""
        return self._text(0)

    @property
    def serial2(self):
        """Text received so far on the mini UART (boot(serial2=True) only)."""
        return self._text(1)

    def lines(self, port=0):
        """Complete lines received so far, without line endings."""
        return complete_lines(self._text(port)).replace("\r", "").splitlines()

    def wait_for(self, pattern, timeout=10.0, port=0, line=False):
        """Wait until `pattern` shows up on the serial stream.

        `pattern` is a substring or a compiled regex. Returns the match
        (a re.Match for regexes, the substring otherwise). With
        line=True only complete lines are searched. Raises QemuError
        with the captured log on timeout or if QEMU exits.
        """
        deadline = time.monotonic() + timeout
        with self._cond:
            while True:
                text = self._bufs[port].decode("utf-8", "replace").replace("\r\n", "\n")
                if line:
                    text = complete_lines(text)
                if isinstance(pattern, re.Pattern):
                    m = pattern.search(text)
                    if m:
                        return m
                elif pattern in text:
                    return pattern
                left = deadline - time.monotonic()
                if left <= 0:
                    break
                if self.qemu.proc.poll() is not None:
                    raise QemuError(f"QEMU exited waiting for {pattern!r}.\n"
                                    f"stderr: {self.qemu.stderr()}\nserial:\n{text}")
                self._cond.wait(min(left, 0.2))
        raise QemuError(self._with_state(
            f"timeout ({timeout}s) waiting for {pattern!r}. serial:\n{text}"))

    def send(self, text, port=0):
        """Write text to the guest's serial input."""
        data = text.encode() if isinstance(text, str) else text
        self._socks[port].sendall(data)

    def script(self, command, timeout=10.0):
        """Send one line on the script channel and return its ScriptReply.

        Needs boot(serial2=True) and input_script_start() in the guest.
        Collects the "| " lines a project handler writes, up to the
        status line. Send one command at a time: a `wait` command answers
        later, so its status line would end the next call early.
        Raises QemuError on timeout or if QEMU exits.
        """
        if "\n" in command or "\r" in command:
            raise ValueError("one command, without a line ending")
        start = len(self.lines(SCRIPT_PORT))
        self.send(command + "\n", port=SCRIPT_PORT)
        deadline = time.monotonic() + timeout
        with self._cond:
            while True:
                lines = complete_lines(self._bufs[SCRIPT_PORT].decode("utf-8", "replace"))
                new = lines.replace("\r", "").splitlines()[start:]
                for i, line in enumerate(new):
                    if is_script_status(line):
                        data = [x[len(SCRIPT_DATA):] for x in new[:i]
                                if x.startswith(SCRIPT_DATA)]
                        return ScriptReply(line, data)
                left = deadline - time.monotonic()
                if left <= 0:
                    msg = (f"timeout ({timeout}s) waiting for the reply to "
                           f"{command!r}. script channel:\n{lines}")
                    break
                if self.qemu.proc.poll() is not None:
                    raise QemuError(f"QEMU exited waiting for the reply to {command!r}.\n"
                                    f"stderr: {self.qemu.stderr()}\nscript channel:\n{lines}")
                self._cond.wait(min(left, 0.2))
        raise QemuError(self._with_state(msg))

    def state_dump(self, title="guest state"):
        """A text snapshot of the guest: CPU samples, IRQ controllers, system timer.

        Samples CPU0's pc, lr, sp and CPSR 5 times, 100 ms apart, then
        reads the interrupt controller and system timer registers over
        HMP. Resolves pc and lr to symbols when the image's ELF is known.
        Never raises: a failure becomes one "state dump failed" line.
        Runs within DUMP_BUDGET seconds. If a QMP read times out, later
        QMP replies on this Vm may be out of step.
        """
        q = self.qemu
        machine = getattr(q, "machine", "?")
        rc = q.proc.poll() if q.proc is not None else None
        if q.proc is None or rc is not None:
            return dump.block([f"not collected: QEMU has exited (status {rc})"],
                              machine, title)
        deadline = time.monotonic() + self.DUMP_BUDGET
        sock = getattr(q, "_qmp", None)

        def hmp(cmd):
            left = deadline - time.monotonic()
            if left <= 0:
                raise QemuError(f"over the {self.DUMP_BUDGET}s budget before {cmd!r}")
            if q.proc.poll() is not None:
                raise QemuError(f"QEMU exited during the dump ({q.proc.returncode})")
            if sock is not None:
                sock.settimeout(min(left, self.DUMP_READ_TIMEOUT))
            return q.hmp(cmd)

        try:
            lines = dump.collect(hmp, machine, self.elf)
        except Exception as e:  # noqa: BLE001 - never mask the caller's error
            lines = [f"state dump failed: {type(e).__name__}: {e}"]
        finally:
            if sock is not None:
                try:
                    sock.settimeout(q.timeout)
                except OSError:
                    pass
        return dump.block(lines, machine, title)

    def _with_state(self, message):
        """`message` followed by the guest state block. Never raises."""
        try:
            return message + "\n" + self.state_dump("guest state at timeout")
        except Exception as e:  # noqa: BLE001
            return message + f"\nstate dump failed: {type(e).__name__}: {e}"

    def markers(self, port=0):
        """All complete `@RC` marker lines seen so far, as dicts."""
        return parse_markers(self._text(port))

    def screendump(self, path):
        """Write the screen to `path` as PPM."""
        self.qemu.execute("screendump", filename=os.fspath(path))
        return path

    def close(self):
        for s in self._socks:
            with contextlib.suppress(OSError):
                s.shutdown(socket.SHUT_RDWR)
            s.close()
        self.qemu.close()
        for t in self._threads:
            t.join(2)


def _connect_unix(path, qemu, timeout):
    deadline = time.monotonic() + timeout
    while True:
        if qemu.proc.poll() is not None:
            raise QemuError(f"QEMU exited ({qemu.proc.returncode}): {qemu.stderr()}")
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            s.connect(path)
            return s
        except OSError:
            s.close()
            if time.monotonic() > deadline:
                raise QemuError(f"connect timeout: {path}")
            time.sleep(0.02)


def boot_args(image, board, mode="auto", sd=None):
    """QEMU arguments (after -M) for a firmware-like boot of `image`."""
    b = BOARDS[board]
    if mode == "auto":
        mode = b.auto_mode
    if mode not in ("svc", "hyp"):
        raise ValueError(f"mode must be auto, svc or hyp, not {mode!r}")
    if mode == "hyp" and b.auto_mode == "svc":
        raise ValueError(f"{board} has no HYP mode")
    flags = FLAG_HYP if (mode == "hyp" and not b.a64) else 0
    stub = fwstub_path()
    args = ["-device", f"loader,file={stub},addr=0x0,force-raw=on",
            "-device", f"loader,file={os.fspath(image)},addr={KERNEL_ADDR:#x},force-raw=on"]
    args += loader_words(PARAMS_ADDR, [PARAMS_MAGIC, b.mach_id, b.mem, flags])
    if b.a64:
        args += loader_words(a64_shim.ADDR, a64_shim.words(mode), entry=a64_shim.ADDR)
    # Without a kernel, AArch32 cores reset at 0x0, where the stub is.
    # (A loader with addr=0 means "no address", so it cannot set the PC.)
    if sd is not None:
        args += ["-drive", f"if=sd,format=raw,file={os.fspath(sd)}"]
    return args


@contextlib.contextmanager
def boot(image, board, *, mode="auto", sd=None, serial2=False, extra_args=None,
         timeout=10.0, elf=None):
    """Boot `image` on `board` and yield a Vm. QEMU is always killed on exit.

    elf is the image's ELF, used to name functions in the state dump.
    By default it is the .elf next to the .img, if one exists.
    """
    if board not in BOARDS:
        raise ValueError(f"unknown board {board!r}, expected one of {sorted(BOARDS)}")
    args = boot_args(image, board, mode, sd)
    q = Qemu(BOARDS[board].binary, board, timeout=timeout, null_serial=False)
    ports = 2 if serial2 else 1
    paths = [q._sock(f"serial{i}") for i in range(ports)]
    # Start stopped, connect serial and QMP, then run: no output is lost.
    serial = []
    for i, p in enumerate(paths):
        serial += ["-chardev", f"socket,id=ser{i},path={p},server=on,wait=off",
                   "-serial", f"chardev:ser{i}"]
    q.extra = serial + ["-S"] + args + list(extra_args or [])
    vm = None
    try:
        q.start()
        socks = [_connect_unix(p, q, timeout) for p in paths]
        vm = Vm(q, socks, elf if elf is not None else dump.find_elf(image))
        q.execute("cont")
        yield vm
    finally:
        if vm is not None:
            vm.close()
        else:
            q.close()

