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
import threading
import time

# Keep-alive: a harmless QMP command at a fixed interval while the VM runs.
# See tests/README.md, "QEMU keep-alive".
KEEPALIVE_ENV = "ROBCO_QEMU_KEEPALIVE"
KEEPALIVE_INTERVAL = 0.02
KEEPALIVE_TIMEOUT = 1.0


def keepalive_default():
    """False when ROBCO_QEMU_KEEPALIVE is 0, off, no or false. On otherwise."""
    return os.environ.get(KEEPALIVE_ENV, "1").strip().lower() not in ("0", "off", "no", "false")


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
                 serial_count=0, timeout=20.0, null_serial=True):
        self.binary = binary
        self.machine = machine
        self.extra = list(args)
        self.use_qmp = qmp
        self.use_qtest = qtest
        self.serial_count = serial_count
        self.null_serial = null_serial  # False when the caller adds its own -serial
        self.timeout = timeout
        self.tmpdir = tempfile.mkdtemp(prefix="qh")
        self.proc = None
        self._qmp = None
        self._qmp_buf = b""  # bytes read past the last complete QMP line
        self._qmp_seq = 0
        # One lock serialises every QMP exchange: a command and its reply,
        # or one read in wait_event(). The keep-alive thread takes it too.
        self._qmp_lock = threading.RLock()
        self.vm_running = False  # set by stop/cont and the STOP/RESUME events
        self.keepalive_on = False
        self.keepalive_pings = 0
        self._ka_thread = None
        self._ka_stop = threading.Event()
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
        elif self.null_serial:
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
        self._qmp, self._qmp_buf = s, b""
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

    @classmethod
    def run_to_exit(cls, binary, machine, args=(), timeout=20.0):
        """Run QEMU without QMP and wait for it to exit by itself.

        For configurations QEMU must refuse. Returns (returncode, stderr).
        """
        q = cls(binary, machine, args, qmp=False, timeout=timeout)
        try:
            q.start()
            return q.wait_exit()
        finally:
            q.close()

    def close(self):
        self.stop_keepalive()
        for f in (self._qmp, self._qt_file, self._qt):
            try:
                if f:
                    f.close()
            except OSError:
                pass
        try:
            if self.proc:
                self._stop_proc()
        finally:
            shutil.rmtree(self.tmpdir, ignore_errors=True)

    def _signal_group(self, sig):
        # The process may exit at any time, for example after a guest
        # power-off. macOS then reports EPERM for the group, not ESRCH.
        if self.proc.poll() is not None:
            return
        try:
            os.killpg(self.proc.pid, sig)
        except (ProcessLookupError, PermissionError):
            pass

    def _stop_proc(self):
        """Stop and reap QEMU. Safe when it has already exited."""
        self._signal_group(signal.SIGTERM)
        try:
            self.proc.wait(3)
            return
        except subprocess.TimeoutExpired:
            pass
        self._signal_group(signal.SIGKILL)
        try:
            self.proc.wait(3)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.close()

    # -- keep-alive ------------------------------------------------------

    def start_keepalive(self, interval=KEEPALIVE_INTERVAL):
        """Send query-status every `interval` seconds while the VM runs.

        Runs on a daemon thread until stop_keepalive() or close(). Pings
        pause while the VM is stopped and end when QEMU exits or the QMP
        connection fails. Replies are matched by id like any command.
        """
        if self._ka_thread is not None or self._qmp is None:
            return
        self._ka_stop.clear()
        self.keepalive_on = True
        self._ka_thread = threading.Thread(target=self._keepalive_loop, args=(interval,),
                                           name="qmp-keepalive", daemon=True)
        self._ka_thread.start()

    def stop_keepalive(self):
        t, self._ka_thread = self._ka_thread, None
        self._ka_stop.set()
        if t is not None and t is not threading.current_thread():
            t.join(KEEPALIVE_TIMEOUT + 1)

    def _keepalive_loop(self, interval):
        while not self._ka_stop.wait(interval):
            if self.proc is not None and self.proc.poll() is not None:
                return
            try:
                with self._qmp_lock:  # the flag is checked and used in one hold
                    if not self.vm_running:
                        continue
                    self.command("query-status", timeout=KEEPALIVE_TIMEOUT)
                    self.keepalive_pings += 1
            except socket.timeout:
                continue  # the late reply is dropped by its id
            except (QemuError, OSError, ValueError, AttributeError):
                return  # connection gone; close() is on its way

    # -- QMP / HMP -------------------------------------------------------

    def _qmp_read(self):
        """Read one QMP message. Raises socket.timeout and keeps partial input.

        Reads the socket directly and buffers bytes past the line, so a
        timeout loses nothing and the connection stays usable.
        """
        while b"\n" not in self._qmp_buf:
            chunk = self._qmp.recv(65536)
            if not chunk:
                raise QemuError("QMP connection closed: " + self.stderr())
            self._qmp_buf += chunk
        line, _, self._qmp_buf = self._qmp_buf.partition(b"\n")
        msg = json.loads(line)
        if msg.get("event") == "STOP":
            self.vm_running = False
        elif msg.get("event") == "RESUME":
            self.vm_running = True
        return msg

    def execute(self, cmd, **arguments):
        """Run a QMP command. Returns 'return', raises QemuError on 'error'.

        Each command carries a unique id. Replies with another id are late
        answers to commands that timed out and are dropped. Events are
        queued for wait_event().
        """
        return self.command(cmd, arguments)

    def command(self, cmd, arguments=None, timeout=None):
        """execute() with arguments as a dict and an optional read timeout.

        Holds the QMP lock from the send to the reply, so the keep-alive
        thread and the caller never read each other's replies.
        """
        with self._qmp_lock:
            old = self._qmp.gettimeout()
            if timeout is not None:
                self._qmp.settimeout(timeout)
            try:
                result = self._command(cmd, arguments)
            finally:
                if timeout is not None:
                    self._qmp.settimeout(old)
            if cmd == "stop":
                self.vm_running = False
            elif cmd == "cont":
                self.vm_running = True
            return result

    def _command(self, cmd, arguments):
        self._qmp_seq += 1
        req_id = f"rc-{self._qmp_seq}"
        msg = {"execute": cmd, "id": req_id}
        if arguments:
            msg["arguments"] = arguments
        self._qmp.sendall(json.dumps(msg).encode() + b"\n")
        while True:
            resp = self._qmp_read()
            if "event" in resp:
                self.events.append(resp)
            elif resp.get("id") != req_id:
                continue  # stale reply, or a message without an id
            elif "error" in resp:
                raise QemuError(f"{cmd}: {resp['error'].get('desc')}")
            elif "return" in resp:
                return resp["return"]

    def wait_event(self, name, timeout=5.0):
        """Return the first QMP event called `name` (already queued or new)."""
        deadline = time.monotonic() + timeout
        while True:
            with self._qmp_lock:
                for ev in self.events:
                    if ev["event"] == name:
                        self.events.remove(ev)
                        return ev
                left = deadline - time.monotonic()
                if left <= 0:
                    raise QemuError(f"no {name} event within {timeout}s")
                # Short reads, so the keep-alive thread gets the lock between them.
                self._qmp.settimeout(min(left, 0.05))
                try:
                    resp = self._qmp_read()
                except socket.timeout:
                    continue
                finally:
                    self._qmp.settimeout(self.timeout)
                if "event" in resp:
                    self.events.append(resp)

    def hmp(self, command, timeout=None):
        """Run an HMP (human monitor) command through QMP and return its text."""
        return self.command("human-monitor-command", {"command-line": command}, timeout)

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
