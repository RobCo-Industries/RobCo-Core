"""Minimal QEMU driver for tests. Stdlib only.

Usage:
    with Qemu("qemu-system-arm", "raspi0", qtest=True) as q:
        q.qtest_writel(0x2000B880 + 0x20, 0x1008)
        print(q.hmp("info roms"))
"""

import json
import math
import os
import select
import shutil
import signal
import socket
import stat
import subprocess
import sys
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


# Each VM's temp folder holds this file: "v2 <qemu pid> <start time>
# <owner pid> <pgid>". The start time is QEMU's own, as process_start()
# reads it, or "-" when it could not. sweep_stale() reads it and skips any
# other form. See tests/README.md, "QEMU outlives no test process".
PIDFILE = "qemu.pid"
PIDFILE_TAG = "v2"
TMP_PREFIX = "qh"
# A watcher per VM: it leads a new process group and QEMU starts inside
# that group. It blocks on a pipe from the test process and, at end of
# file, kills its own group: QEMU, QEMU's children and itself. The test
# process dying for any reason, SIGKILL included, closes the pipe.
#
# No moment exists with QEMU up and no watcher: the watcher starts first,
# QEMU's child joins its group, then waits in LAUNCHER until the test
# process, still alive, lets it exec QEMU. The group number
# cannot name a stranger: a group id stays taken while its leader lives,
# and the watcher outlives QEMU. close() signals QEMU's own pid, never
# the group, so the watcher gets nothing from it. close() reaps QEMU
# first, then closes the pipe and reaps the watcher.
#
# The watcher ignores SIGINT, SIGHUP and SIGTERM, so a signal sent to the
# whole group from outside leaves it in place. Only SIGKILL stops it
# alone. With preexec_fn the child ignores them before the exec. With
# process_group= the shell's trap does it, and the watcher prints one
# byte after its trap: start() waits for that byte before QEMU starts.
# See tests/README.md, "QEMU outlives no test process".
WATCHER_SIGNALS = (signal.SIGINT, signal.SIGHUP, signal.SIGTERM)
WATCHER = "trap '' INT HUP TERM; echo; read _ ; kill -s KILL 0"
# QEMU's child runs this shell first, already inside the watcher's group.
# It waits for one line from the test process, then execs QEMU. The test
# process sends it only after Popen returned, that is after the join, and
# while it lives, so the watcher lives too. A test process that dies first
# closes the pipe: the shell exits and QEMU never runs.
# See tests/README.md, "QEMU outlives no test process".
LAUNCHER = 'read _ || exit 125; exec "$@" </dev/null'

# Popen(process_group=) exists from Python 3.11. Older ones call
# os.setpgid() from preexec_fn: same call, same child, before the exec.
# Set ROBCO_QEMU_PGID_FALLBACK=1 to force the older form, for its tests.
PGID_FALLBACK_ENV = "ROBCO_QEMU_PGID_FALLBACK"
USE_PROCESS_GROUP = (sys.version_info >= (3, 11)
                     and os.environ.get(PGID_FALLBACK_ENV, "") != "1")


def _group_kwargs(pgid, ignore=()):
    """Popen arguments that put the child in group `pgid`, 0 for a new one.

    The preexec_fn form also sets the signals in `ignore` to SIG_IGN in
    the child before the exec. An ignored signal stays ignored across
    the exec. The process_group form cannot: the caller must ignore them
    in the child some other way.
    """
    if USE_PROCESS_GROUP:
        return {"process_group": pgid}

    def join():
        os.setpgid(0, pgid)
        for sig in ignore:
            signal.signal(sig, signal.SIG_IGN)
    return {"preexec_fn": join}


def keepalive_default():
    """False when ROBCO_QEMU_KEEPALIVE is 0, off, no or false. On otherwise."""
    return os.environ.get(KEEPALIVE_ENV, "1").strip().lower() not in ("0", "off", "no", "false")


class QemuError(RuntimeError):
    pass


# ps answers in well under a second. A hung ps must not hold up the session.
PS_TIMEOUT = 2.0
PROC = "/proc"  # tests point it elsewhere to take the ps path


def _report(msg):
    """The harness log: one line on stderr."""
    try:
        sys.stderr.write(f"robco harness: {msg}\n")
    except (OSError, ValueError):
        pass


def _run_ps(pid):
    """Run ps for the start time of `pid`. Tests replace it."""
    return subprocess.run(["ps", "-o", "lstart=", "-p", str(pid)], capture_output=True,
                          text=True, timeout=PS_TIMEOUT,
                          env={"LC_ALL": "C", "PATH": "/bin:/usr/bin"})


def _valid_pid(pid):
    """True for an int that names one process and no group: above 1."""
    return isinstance(pid, int) and not isinstance(pid, bool) and pid > 1


def _run_ps_stat(pid):
    """Run ps for the state of `pid`. Tests replace it."""
    return subprocess.run(["ps", "-o", "stat=", "-p", str(pid)], capture_output=True,
                          text=True, timeout=PS_TIMEOUT,
                          env={"LC_ALL": "C", "PATH": "/bin:/usr/bin"})


def zombie(pid):
    """True when `pid` names a zombie: dead, not yet reaped.

    A container whose process 1 reaps no child keeps every orphan that
    dies as a zombie. It holds no CPU, no memory and no file, so it counts
    as dead. Linux reads /proc, macOS asks ps. False when unsure.
    """
    try:
        with open(f"{PROC}/{pid}/stat") as f:
            return f.read().rpartition(")")[2].split()[0] in ("Z", "X")
    except FileNotFoundError:
        if os.path.isdir(f"{PROC}/self"):
            return False
    except (OSError, ValueError, IndexError):
        return False
    try:
        r = _run_ps_stat(pid)
        return r.returncode == 0 and r.stdout.strip().startswith("Z")
    except (OSError, subprocess.SubprocessError, AttributeError):
        return False


def process_start(pid):
    """When `pid` started, in seconds since the epoch, or None.

    None when the process is gone or a zombie, and whenever the start
    time cannot be learned: no /proc, no ps, ps not allowed, a hung ps,
    odd output. Linux reads /proc, so the CI container needs no ps. macOS asks ps.
    Both are good to about a second.
    """
    try:
        pid = int(pid)
    except (TypeError, ValueError, OverflowError):
        return None
    if not _valid_pid(pid) or zombie(pid):
        return None
    try:
        with open(f"{PROC}/{pid}/stat") as f:
            ticks = int(f.read().rpartition(")")[2].split()[19])
        with open(f"{PROC}/stat") as f:
            btime = next(int(x.split()[1]) for x in f if x.startswith("btime "))
        return btime + ticks / os.sysconf("SC_CLK_TCK")
    except FileNotFoundError:
        if os.path.isdir(f"{PROC}/self"):
            return None
    except (OSError, ValueError, IndexError, StopIteration, ZeroDivisionError):
        return None
    try:
        r = _run_ps(pid)
        if r.returncode != 0:
            return None
        start = time.mktime(time.strptime(r.stdout.strip(), "%a %b %d %H:%M:%S %Y"))
    except (OSError, subprocess.SubprocessError, ValueError, OverflowError,
            TypeError, AttributeError):
        return None
    return start if math.isfinite(start) else None


def _alive(pid):
    """False only when `pid` surely names no process or a zombie. Never signals."""
    if not _valid_pid(pid):
        return True  # not a process we may ask about: count it as alive
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except OSError:
        return True
    return not zombie(pid)


def sweep_stale(tmp=None, slack=2.0):
    """Kill the QEMU of each VM an earlier, dead test process left.

    Reads the pidfile in every qh* folder of `tmp`. Kills a QEMU only when
    its owner is gone, it still leads the recorded process group and it
    started at the recorded time, so a reused pid or a developer's own
    QEMU is never touched. Removes the folders of dead VMs. Returns the
    killed pids.

    Never raises. A pidfile it cannot judge, for example because ps may
    not run, stays in place and its VM lives. It reports the skipped
    pidfiles once, on the harness log.
    """
    killed, skipped = [], []
    try:
        tmp = tmp or tempfile.gettempdir()
        names = [n for n in os.listdir(tmp) if n.startswith(TMP_PREFIX)]
    except Exception:  # noqa: BLE001 - the sweep is a courtesy
        return killed
    for name in names:
        folder = os.path.join(tmp, name)
        try:
            outcome = _sweep_one(folder, slack)
        except Exception:  # noqa: BLE001 - skip this pidfile, keep going
            outcome = "skipped"
        if outcome == "skipped":
            skipped.append(folder)
        elif isinstance(outcome, int):
            killed.append(outcome)
    if skipped:
        _report(f"left {len(skipped)} QEMU pidfile(s) it could not check, "
                f"for example {skipped[0]}")
    return killed


def _owned(st, uid, kind):
    """True when stat result `st` is of `kind` (stat.S_ISDIR or S_ISREG),
    owned by `uid` and not writable by group or others."""
    return (kind(st.st_mode) and st.st_uid == uid
            and not st.st_mode & (stat.S_IWGRP | stat.S_IWOTH))


def _read_own_pidfile(folder):
    """The pidfile's text, or None when the folder or the file is not
    provably this user's own. Follows no symbolic link, and reads from the
    descriptor it checked, so nothing can swap the file in between.
    Raises FileNotFoundError when the folder holds no pidfile.

    os.getuid() is POSIX only: this harness does not run on Windows.
    """
    uid = os.getuid()
    if not _owned(os.lstat(folder), uid, stat.S_ISDIR):
        return None
    fd = os.open(os.path.join(folder, PIDFILE), os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        if not _owned(os.fstat(fd), uid, stat.S_ISREG):
            return None
        with os.fdopen(fd, encoding="utf-8") as f:
            fd = None
            return f.read(4096)
    finally:
        if fd is not None:
            os.close(fd)


def _run_ps_uid(pid):
    """Run ps for the user id of `pid`. Tests replace it."""
    return subprocess.run(["ps", "-o", "uid=", "-p", str(pid)], capture_output=True,
                          text=True, timeout=PS_TIMEOUT,
                          env={"LC_ALL": "C", "PATH": "/bin:/usr/bin"})


def _process_uid(pid):
    """The user id that owns `pid`, or None when it cannot be read.
    Linux reads the owner of /proc/<pid>, macOS asks ps."""
    try:
        return os.stat(f"{PROC}/{pid}").st_uid
    except FileNotFoundError:
        if os.path.isdir(f"{PROC}/self"):
            return None
    except OSError:
        return None
    try:
        r = _run_ps_uid(pid)
        return int(r.stdout.strip()) if r.returncode == 0 else None
    except (OSError, subprocess.SubprocessError, ValueError, AttributeError):
        return None


def _sweep_one(folder, slack):
    """Judge one VM folder. Returns the killed pid, "skipped" or None."""
    try:
        text = _read_own_pidfile(folder)
        if text is None:
            return "skipped"  # not provably ours: someone else may have made it
        tag, pid, started, owner, pgid = text.split()
        if tag != PIDFILE_TAG:
            return "skipped"  # an older form: its start time is not QEMU's
        pid, started, owner, pgid = int(pid), float(started), int(owner), int(pgid)
    except FileNotFoundError:
        return None  # a folder with no pidfile: not a VM, or not written yet
    except (OSError, ValueError, UnicodeDecodeError):
        return "skipped"
    if not (_valid_pid(pid) and _valid_pid(owner) and _valid_pid(pgid)
            and math.isfinite(started)):
        return "skipped"
    if owner == os.getpid():
        return None  # our own VM
    owner_start = process_start(owner)
    if owner_start is None:
        if _alive(owner):
            return None  # an owner we cannot read counts as alive
    elif owner_start <= started + slack:
        return None  # the owner, or a process that could be it, still runs
    actual = process_start(pid)
    if actual is None:
        if _alive(pid):
            return "skipped"  # alive but unproven: never killed
        shutil.rmtree(folder, ignore_errors=True)  # the VM is gone
        return None
    try:
        same = os.getpgid(pid) == pgid and abs(actual - started) <= slack
    except OSError:
        same = False
    if not same:
        return None  # the pid now names another process: not ours
    if _process_uid(pid) != os.getuid():
        return "skipped"  # another user's process, or one we cannot read
    try:
        os.kill(pid, signal.SIGKILL)
    except OSError:
        return "skipped"
    shutil.rmtree(folder, ignore_errors=True)
    return pid


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


def parse_hmp_registers(text):
    """`info registers` text to a dict of QEMU register name -> int."""
    regs = {}
    for tok in text.replace("\r", "").split():
        if "=" in tok:
            k, _, v = tok.partition("=")
            try:
                regs[k] = int(v, 16)
            except ValueError:
                pass
    return regs


class Qemu:
    def __init__(self, binary, machine, args=(), qmp=True, qtest=False,
                 serial_count=0, timeout=20.0, null_serial=True, qtest_accel="qtest"):
        self.binary = binary
        self.machine = machine
        self.extra = list(args)
        self.use_qmp = qmp
        self.use_qtest = qtest
        # None keeps the default accelerator, so the guest runs and qtest
        # only drives device inputs (set_irq_in).
        self.qtest_accel = qtest_accel
        self.serial_count = serial_count
        self.null_serial = null_serial  # False when the caller adds its own -serial
        self.timeout = timeout
        self.tmpdir = tempfile.mkdtemp(prefix=TMP_PREFIX)
        self.proc = None
        self._watcher = None
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
            cmd += ["-qtest", f"unix:{self._sock('qtest')}"]
            if self.qtest_accel:
                cmd += ["-accel", self.qtest_accel]
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
        try:
            self._start_watcher()
            self._spawn()
            self._write_pidfile()
            self._release()
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

    def _start_watcher(self):
        """Start the watcher as the leader of QEMU's future process group.

        Same session as this process: setpgid() joins only a group of
        the caller's session. Popen returns after the exec, so the group
        exists before QEMU starts. The watcher prints one byte once its
        trap is set. Waiting for it closes the window where a signal to
        the group would still kill the watcher.
        """
        try:
            self._watcher = subprocess.Popen(
                ["/bin/sh", "-c", WATCHER, "qemu-watcher"],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                **_group_kwargs(0, WATCHER_SIGNALS))
        except (OSError, subprocess.SubprocessError) as e:
            # No watcher, no guarantee that QEMU dies with this process.
            raise QemuError(f"cannot start the QEMU watcher with /bin/sh: {e}") from e
        out = self._watcher.stdout
        try:
            ready, _, _ = select.select([out], [], [], self.timeout)
            if not ready or not os.read(out.fileno(), 1):
                raise QemuError("the QEMU watcher did not get ready")
        finally:
            out.close()

    def _spawn(self):
        """Fork QEMU's child into the watcher's group, held before QEMU runs.

        Popen returns after the child joined the group and exec'd the
        launcher shell, which then waits for _release(). QEMU keeps the
        child's pid, so the pidfile can name it before QEMU runs.
        """
        cmd = self.command_line()
        if shutil.which(cmd[0]) is None:
            raise QemuError(f"cannot start QEMU: no executable {cmd[0]}")
        err = open(self.stderr_path, "wb")
        try:
            self.proc = subprocess.Popen(
                ["/bin/sh", "-c", LAUNCHER, "qemu-launcher", *cmd],
                stdin=subprocess.PIPE, stdout=err, stderr=err,
                **_group_kwargs(self._watcher.pid))
        except (OSError, subprocess.SubprocessError) as e:
            # setpgid() failed in the child, or no shell: QEMU never ran.
            raise QemuError(f"cannot start QEMU: {e}") from e
        finally:
            err.close()

    def _release(self):
        """Let the launcher exec QEMU. The child is in the group by now.

        A watcher that died before this point, even one not reaped yet,
        refuses the start: QEMU would run with nothing to stop it.
        """
        go, self.proc.stdin = self.proc.stdin, None
        try:
            if self._watcher.poll() is not None:
                raise QemuError("cannot start QEMU: the watcher is gone")
            go.write(b"\n")
            go.flush()
        except OSError as e:
            raise QemuError(f"cannot start QEMU: {e}") from e
        finally:
            try:
                go.close()
            except OSError:
                pass

    def _write_pidfile(self):
        """Record the VM for sweep_stale(). A failure only warns: the
        watcher still stops QEMU with this process.

        The start time is QEMU's own, read the way the sweep reads it, so
        the two compare like with like. When it cannot be read, "-" goes
        in its place and the sweep never kills this VM.
        """
        started = process_start(self.proc.pid)
        started = "-" if started is None else f"{started:.3f}"
        tmp = os.path.join(self.tmpdir, PIDFILE + ".tmp")
        try:
            # 0600 whatever the umask: the sweep trusts no pidfile that
            # group or others may write.
            fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
            os.fchmod(fd, 0o600)
            with os.fdopen(fd, "w") as f:
                f.write(f"{PIDFILE_TAG} {self.proc.pid} {started} {os.getpid()} "
                        f"{self._watcher.pid}\n")
            os.replace(tmp, os.path.join(self.tmpdir, PIDFILE))
        except OSError as e:
            _report(f"cannot write {PIDFILE} in {self.tmpdir}: {e}")

    def _stop_watcher(self):
        """Close the pipe: the watcher kills what is left of its group and exits."""
        w, self._watcher = self._watcher, None
        if w is None:
            return
        try:
            w.stdin.close()
        except OSError:
            pass  # the watcher is gone: the pipe has no reader
        try:
            w.wait(3)
        except subprocess.TimeoutExpired:
            w.kill()
            w.wait()

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
            # QEMU first: the watcher must outlive it, so a test process
            # killed while close() waits still takes QEMU with it.
            if self.proc:
                self._stop_proc()
        finally:
            try:
                self._stop_watcher()
            finally:
                shutil.rmtree(self.tmpdir, ignore_errors=True)

    def _stop_proc(self):
        """Stop and reap QEMU. Safe when it has already exited.

        Signals QEMU's own pid, never the group: the watcher must stay to
        kill QEMU if this process dies during a wait. Popen skips the
        signal once QEMU is reaped, so the pid names no stranger. QEMU's
        children, if any, die when the watcher kills the group.
        """
        if self.proc.stdin is not None:
            self.proc.stdin.close()  # never released: the launcher exits
            self.proc.stdin = None
        try:
            self.proc.terminate()
            self.proc.wait(3)
            return
        except subprocess.TimeoutExpired:
            pass
        except OSError:
            pass  # exited between the poll and the signal
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
        """Parse `info registers` into a dict of name -> int (CPU0).

        The keys are QEMU's names. AArch32 state gives R00..R15 and PSR.
        AArch64 state gives X00..X30, SP, PC and PSTATE, with no R15:
        raspi3b runs its AArch64 shim first. Use registers_a32() to read
        AArch32 names right after start.
        """
        return parse_hmp_registers(self.hmp("info registers"))

    def registers_a32(self, timeout=5.0, interval=0.01):
        """Wait until CPU0 is in AArch32 state, then return registers().

        Raises QemuError with the last dump if CPU0 is still in AArch64
        state after `timeout` seconds.
        """
        deadline = time.monotonic() + timeout
        while True:
            text = self.hmp("info registers")
            regs = parse_hmp_registers(text)
            if "R15" in regs:
                return regs
            if time.monotonic() >= deadline:
                raise QemuError(f"CPU0 not in AArch32 state after {timeout} s; "
                                f"last `info registers`:\n{text.strip()}")
            time.sleep(interval)

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

    def set_irq_in(self, path, num, level, name="unnamed-gpio-in"):
        """Drive GPIO input `num` of the device at QOM `path` to `level`."""
        self.qtest(f"set_irq_in {path} {name} {num} {int(level)}")

    def clock_step(self, ns=None):
        return int(self.qtest("clock_step" + (f" {ns}" if ns else "")))
