"""Minimal QEMU driver for tests. Stdlib only.

Usage:
    with Qemu("qemu-system-arm", "raspi0", qtest=True) as q:
        q.qtest_writel(0x2000B880 + 0x20, 0x1008)
        print(q.hmp("info roms"))
"""

import json
import os
import shutil
import signal
import socket
import subprocess
import tempfile
import time


class QemuError(RuntimeError):
    pass


def have_machine(binary, machine):
    """True if `binary` exists and supports `-M machine`."""
    if shutil.which(binary) is None:
        return False
    out = subprocess.run([binary, "-M", "help"], capture_output=True, text=True, timeout=10).stdout
    return any(line.split()[:1] == [machine] for line in out.splitlines())


def loader_words(addr, words, entry=None):
    """-device loader args poking 32-bit words at addr, optionally setting CPU0's PC."""
    args = []
    for i, w in enumerate(words):
        args += ["-device", f"loader,addr={addr + 4 * i:#x},data={w:#010x},data-len=4"]
    if entry is not None:
        args += ["-device", f"loader,addr={entry:#x},cpu-num=0"]
    return args


class Qemu:
    def __init__(self, binary, machine, args=(), qmp=True, qtest=False,
                 serial_count=0, timeout=20.0):
        self.binary = binary
        self.machine = machine
        self.extra = list(args)
        self.use_qmp = qmp
        self.use_qtest = qtest
        self.serial_count = serial_count
        self.timeout = timeout
        self.tmpdir = tempfile.mkdtemp(prefix="qh")
        self.proc = None
        self._qmp = self._qmp_file = None
        self._qt = self._qt_file = None
        self.events = []
        self.serial_paths = [os.path.join(self.tmpdir, f"serial{i}.txt")
                             for i in range(serial_count)]
        self.stderr_path = os.path.join(self.tmpdir, "stderr.txt")

    # -- lifecycle -------------------------------------------------------

    def command_line(self):
        cmd = [self.binary, "-M", self.machine, "-display", "none",
               "-monitor", "none", "-no-user-config"]
        if self.serial_count:
            for p in self.serial_paths:
                cmd += ["-serial", f"file:{p}"]
        else:
            cmd += ["-serial", "null"]
        if self.use_qmp:
            cmd += ["-qmp", f"unix:{self._sock('qmp')},server=on,wait=off"]
        if self.use_qtest:
            cmd += ["-qtest", f"unix:{self._sock('qtest')}", "-accel", "qtest"]
        return cmd + self.extra

    def _sock(self, name):
        return os.path.join(self.tmpdir, name + ".sock")

    def start(self):
        qt_listener = None
        if self.use_qtest:
            # QEMU connects to us as a client for qtest.
            qt_listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            qt_listener.bind(self._sock("qtest"))
            qt_listener.listen(1)
            qt_listener.settimeout(self.timeout)
        err = open(self.stderr_path, "wb")
        self.proc = subprocess.Popen(self.command_line(), stdin=subprocess.DEVNULL,
                                     stdout=err, stderr=err, start_new_session=True)
        err.close()
        try:
            if qt_listener:
                try:
                    self._qt, _ = qt_listener.accept()
                except socket.timeout:
                    raise QemuError("qtest connect timeout: " + self.stderr())
                self._qt.settimeout(self.timeout)
                self._qt_file = self._qt.makefile("rwb", buffering=0)
            if self.use_qmp:
                self._connect_qmp()
        except Exception:
            self.close()
            raise
        finally:
            if qt_listener:
                qt_listener.close()
        return self

    def _connect_qmp(self):
        deadline = time.monotonic() + self.timeout
        path = self._sock("qmp")
        while True:
            if self.proc.poll() is not None:
                raise QemuError(f"QEMU exited ({self.proc.returncode}): {self.stderr()}")
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(path)
                break
            except OSError:
                s.close()
                if time.monotonic() > deadline:
                    raise QemuError("QMP connect timeout")
                time.sleep(0.02)
        s.settimeout(self.timeout)
        self._qmp, self._qmp_file = s, s.makefile("rwb", buffering=0)
        self._qmp_read()  # greeting
        self.execute("qmp_capabilities")

    def stderr(self):
        try:
            with open(self.stderr_path, "r", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def wait_exit(self, timeout=None):
        """Wait for QEMU to exit by itself. Returns (returncode, stderr)."""
        try:
            self.proc.wait(timeout or self.timeout)
        except subprocess.TimeoutExpired:
            raise QemuError("QEMU did not exit in time")
        return self.proc.returncode, self.stderr()

    def close(self):
        for f in (self._qmp_file, self._qmp, self._qt_file, self._qt):
            try:
                if f:
                    f.close()
            except OSError:
                pass
        if self.proc and self.proc.poll() is None:
            try:
                os.killpg(self.proc.pid, signal.SIGTERM)
                self.proc.wait(3)
            except (subprocess.TimeoutExpired, ProcessLookupError):
                try:
                    os.killpg(self.proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                self.proc.wait(3)
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.close()

    # -- QMP / HMP -------------------------------------------------------

    def _qmp_read(self):
        line = self._qmp_file.readline()
        if not line:
            raise QemuError("QMP connection closed: " + self.stderr())
        return json.loads(line)

    def execute(self, cmd, **arguments):
        """Run a QMP command. Returns 'return', raises QemuError on 'error'."""
        msg = {"execute": cmd}
        if arguments:
            msg["arguments"] = arguments
        self._qmp_file.write(json.dumps(msg).encode() + b"\n")
        while True:
            resp = self._qmp_read()
            if "event" in resp:
                self.events.append(resp)
            elif "error" in resp:
                raise QemuError(f"{cmd}: {resp['error'].get('desc')}")
            elif "return" in resp:
                return resp["return"]

    def wait_event(self, name, timeout=5.0):
        """Return the first QMP event called `name` (already queued or new)."""
        deadline = time.monotonic() + timeout
        while True:
            for ev in self.events:
                if ev["event"] == name:
                    self.events.remove(ev)
                    return ev
            left = deadline - time.monotonic()
            if left <= 0:
                raise QemuError(f"no {name} event within {timeout}s")
            self._qmp.settimeout(left)
            try:
                resp = self._qmp_read()
            except socket.timeout:
                continue
            finally:
                self._qmp.settimeout(self.timeout)
            if "event" in resp:
                self.events.append(resp)

    def hmp(self, command):
        """Run an HMP (human monitor) command through QMP and return its text."""
        return self.execute("human-monitor-command", **{"command-line": command})

    def registers(self):
        """Parse `info registers` into a dict of name -> int (CPU0)."""
        regs = {}
        for tok in self.hmp("info registers").replace("\r", "").split():
            if "=" in tok:
                k, _, v = tok.partition("=")
                try:
                    regs[k] = int(v, 16)
                except ValueError:
                    pass
        return regs

    # -- qtest -----------------------------------------------------------

    def qtest(self, line):
        self._qt_file.write(line.encode() + b"\n")
        while True:
            resp = self._qt_file.readline().decode().strip()
            if not resp:
                raise QemuError("qtest connection closed: " + self.stderr())
            if resp.startswith("IRQ"):
                continue
            if not resp.startswith("OK"):
                raise QemuError(f"qtest {line!r}: {resp}")
            return resp[2:].strip()

    def readl(self, addr):
        return int(self.qtest(f"readl {addr:#x}"), 16)

    def writel(self, addr, value):
        self.qtest(f"writel {addr:#x} {value:#x}")

    def clock_step(self, ns=None):
        return int(self.qtest("clock_step" + (f" {ns}" if ns else "")))
