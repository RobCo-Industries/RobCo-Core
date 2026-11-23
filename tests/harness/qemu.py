"""Minimal QEMU driver for tests. Stdlib only.

Usage:
    with Qemu("qemu-system-arm", "raspi0", qtest=True) as q:
        q.qtest_writel(0x2000B880 + 0x20, 0x1008)
        print(q.hmp("info roms"))
"""

import json
import os
import select
import shutil
import signal
import socket
import subprocess
import tempfile
import threading
import time

# Keep-alive: a harmless QMP command at a fixed interval.
# See tests/README.md, "QEMU keep-alive".
KEEPALIVE_ENV = "ROBCO_QEMU_KEEPALIVE"
KEEPALIVE_INTERVAL = 0.02
KEEPALIVE_TIMEOUT = 1.0
# Pings a reader sends without waiting for their replies. Past this many
# unanswered ones, QEMU is busy with the monitor and more add nothing.
KEEPALIVE_MAX_UNANSWERED = 8


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
        # Whoever holds it and waits on the socket sends the due pings.
        self._qmp_lock = threading.RLock()
        # Last known run state: stop/cont, STOP/RESUME events, query-status.
        # A cache only: an unread event can make it stale.
        self.vm_running = False
        self.keepalive_pings = 0  # pings sent since the last start_keepalive()
        self._ka_thread = None
        self._ka_interval = KEEPALIVE_INTERVAL
        self._ka_next = 0.0  # monotonic time the next ping is due
        self._ka_seq = 0
        self._ka_unanswered = 0  # pings sent by a reader, reply not read yet
        self._ka_stop = threading.Event()  # one per thread, replaced on start
        self._ka_reason = None  # why the last thread ended by itself, or None
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
        """Send query-status every `interval` seconds.

        Runs until stop_keepalive() or close(), and ends when QEMU exits
        or the QMP connection fails. Pings go on while the VM is stopped:
        query-status changes nothing. While another thread holds the QMP
        lock and waits on the socket, that thread sends the due pings and
        drops their replies by id, so a long command or wait_event() does
        not delay them. Does nothing while a keep-alive is already on.
        Resets the ping count.
        """
        if self.keepalive_on or self._qmp is None:
            return
        self.stop_keepalive()  # reap a thread that ended by itself
        stop = self._ka_stop = threading.Event()
        with self._qmp_lock:
            self.keepalive_pings = 0
            self._ka_unanswered = 0
            self._ka_interval = interval
            self._ka_next = time.monotonic() + interval
        self._ka_reason = None
        self._ka_thread = threading.Thread(target=self._keepalive_loop, args=(interval, stop),
                                           name="qmp-keepalive", daemon=True)
        self._ka_thread.start()

    def stop_keepalive(self):
        """Stop the pings. Safe to call twice, from any thread."""
        t, self._ka_thread = self._ka_thread, None
        self._ka_stop.set()
        if t is not None and t is not threading.current_thread():
            t.join(KEEPALIVE_TIMEOUT + 1)

    @property
    def keepalive_on(self):
        """True while the ping thread is alive and not asked to stop."""
        t = self._ka_thread
        return t is not None and t.is_alive() and not self._ka_stop.is_set()

    def keepalive_status(self):
        """The keep-alive state for the dump, from what the thread does now.

        `on`, `off`, or `off (stopped: <reason>)` when the thread ended
        by itself.
        """
        if self.keepalive_on:
            return "on"
        reason = self._ka_reason
        return f"off (stopped: {reason})" if reason else "off"

    def _keepalive_loop(self, interval, stop):
        try:
            while not stop.wait(max(0.0, self._ka_next - time.monotonic())):
                if self.proc is not None and self.proc.poll() is not None:
                    self._ka_reason = f"QEMU exited ({self.proc.returncode})"
                    return
                try:
                    with self._qmp_lock:
                        if stop.is_set():
                            return
                        if time.monotonic() < self._ka_next:
                            continue  # a reader sent it while we waited for the lock
                        self._ka_next = time.monotonic() + interval
                        self.keepalive_pings += 1
                        # Reads until its own reply, so it also drains the
                        # replies to pings a reader sent.
                        self.command("query-status", timeout=KEEPALIVE_TIMEOUT)
                        # QEMU answers in order: earlier pings got theirs.
                        self._ka_unanswered = 0
                except socket.timeout:
                    continue  # the late reply is dropped by its id
        except Exception as e:  # noqa: BLE001 - the reason goes to the dump
            if not stop.is_set():
                msg = str(e).splitlines()[0][:80].rstrip(": ") if str(e) else ""
                self._ka_reason = f"{type(e).__name__}: {msg}" if msg else type(e).__name__

    # -- QMP / HMP -------------------------------------------------------

    def _ping_due_in(self):
        """Seconds until a reader should send a ping, or None for no ping."""
        if not self.keepalive_on or self._ka_unanswered >= KEEPALIVE_MAX_UNANSWERED:
            return None
        return self._ka_next - time.monotonic()

    def _send_ping(self):
        """Send a keep-alive ping without waiting. Its reply is dropped by id."""
        self._ka_seq += 1
        self._ka_next = time.monotonic() + self._ka_interval
        self._ka_unanswered += 1
        self.keepalive_pings += 1
        self._qmp.sendall(json.dumps({"execute": "query-status",
                                      "id": f"ka-{self._ka_seq}"}).encode() + b"\n")

    def _qmp_read(self):
        """Read one QMP message. Raises socket.timeout and keeps partial input.

        The socket timeout bounds the whole read. Reads the socket directly
        and buffers bytes past the line, so a timeout loses nothing and the
        connection stays usable. Sends keep-alive pings when they are due
        while it waits, so a long wait does not delay them.
        """
        timeout = self._qmp.gettimeout()
        deadline = None if timeout is None else time.monotonic() + timeout
        while b"\n" not in self._qmp_buf:
            due = self._ping_due_in()
            if due is not None and due <= 0:
                self._send_ping()
                continue
            left = None if deadline is None else deadline - time.monotonic()
            if left is not None and left <= 0:
                raise socket.timeout("timed out")
            waits = [w for w in (left, due) if w is not None]
            ready, _, _ = select.select([self._qmp], [], [], min(waits) if waits else None)
            if not ready:
                continue
            chunk = self._qmp.recv(65536)
            if not chunk:
                raise QemuError("QMP connection closed: " + self.stderr())
            self._qmp_buf += chunk
        line, _, self._qmp_buf = self._qmp_buf.partition(b"\n")
        msg = json.loads(line)
        if str(msg.get("id", "")).startswith("ka-") and self._ka_unanswered:
            self._ka_unanswered -= 1
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
                # Short reads let other callers in. Pings do not need it:
                # _qmp_read() sends them while it waits.
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
