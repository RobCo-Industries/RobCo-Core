"""QMP keep-alive and the stop/cont state dump, against a fake QMP server. No QEMU needed."""

import contextlib
import json
import re
import socket
import threading
import time

import pytest

from harness import boot as boot_mod
from harness import qemu as qemu_mod
from harness.boot import Vm
from harness.qemu import Qemu, QemuError

INTERVAL = 0.02


class FakeServer:
    """Plays QEMU's QMP side on a socket pair. Logs (time, command, id).

    reply(req) returns the message to send, or None for no answer.
    By default query-status reports running and other commands return {}.
    """

    def __init__(self, sock, reply=None):
        self.sock = sock
        self.reply = reply or (lambda req: None)
        self.log = []
        self.lock = threading.Lock()
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.thread.start()

    def _serve(self):
        buf = b""
        while True:
            try:
                chunk = self.sock.recv(65536)
            except OSError:
                return
            if not chunk:
                return
            buf += chunk
            while b"\n" in buf:
                line, _, buf = buf.partition(b"\n")
                req = json.loads(line)
                with self.lock:
                    self.log.append((time.monotonic(), req["execute"], req.get("id")))
                msg = self.reply(req)
                if msg is None:
                    msg = default_reply(req)
                if msg is False:
                    continue
                msgs = msg if isinstance(msg, list) else [msg]
                try:
                    self.sock.sendall(b"".join(json.dumps(m).encode() + b"\r\n" for m in msgs))
                except OSError:
                    return

    def commands(self, name=None):
        with self.lock:
            return [c for _, c, _ in self.log if name is None or c == name]

    def times(self, name):
        with self.lock:
            return [t for t, c, _ in self.log if c == name]


def default_reply(req):
    if req["execute"] == "query-status":
        return {"return": {"running": True, "status": "running"}, "id": req.get("id")}
    return {"return": {}, "id": req.get("id")}


class FakeProc:
    returncode = None
    pid = -1

    def poll(self):
        return self.returncode


@contextlib.contextmanager
def wired(reply=None, running=True):
    """A Qemu on one end of a socket pair and a FakeServer on the other."""
    a, b = socket.socketpair()
    q = Qemu("qemu-none", "raspi0", timeout=2.0)
    q._qmp = a
    a.settimeout(q.timeout)
    q.proc = FakeProc()
    q.vm_running = running
    srv = FakeServer(b, reply)
    try:
        yield q, srv
    finally:
        q.stop_keepalive()
        q.proc = None
        a.close()
        b.close()
        q.close()


def test_pings_at_the_interval_during_a_wait():
    with wired() as (q, srv):
        q.start_keepalive(INTERVAL)
        time.sleep(1.0)  # a plain sleep in the caller is covered too
        q.stop_keepalive()
        t = srv.times("query-status")
    # 50 at the nominal rate. The bounds leave room for a loaded host.
    assert 20 <= len(t) <= 55, len(t)
    gaps = sorted(b - a for a, b in zip(t, t[1:]))
    assert INTERVAL * 0.9 <= gaps[len(gaps) // 2] <= INTERVAL * 3, gaps


def test_no_pings_after_the_wait_ends():
    with wired() as (q, srv):
        q.start_keepalive(INTERVAL)
        time.sleep(0.2)
        q.stop_keepalive()
        n = len(srv.commands("query-status"))
        time.sleep(0.2)
        assert n > 0
        assert len(srv.commands("query-status")) == n
        assert not q._ka_thread


def test_no_pings_while_the_vm_is_stopped():
    with wired() as (q, srv):
        q.start_keepalive(INTERVAL)
        time.sleep(0.1)
        q.execute("stop")
        n = len(srv.commands("query-status"))
        time.sleep(0.3)
        assert len(srv.commands("query-status")) == n
        q.execute("cont")
        time.sleep(0.2)
        assert len(srv.commands("query-status")) > n


def test_stop_event_from_the_guest_pauses_pings():
    stop_ev = {"event": "STOP", "data": {}, "timestamp": {"seconds": 0, "microseconds": 0}}
    sent = []

    def reply(req):
        if req["execute"] == "query-status" and not sent:
            sent.append(1)
            return [stop_ev, default_reply(req)]
        return None
    with wired(reply) as (q, srv):
        q.start_keepalive(INTERVAL)
        time.sleep(0.3)
        assert len(srv.commands("query-status")) == 1
        assert q.events == [stop_ev] and not q.vm_running


def test_pings_end_when_qemu_exits():
    with wired() as (q, srv):
        q.start_keepalive(INTERVAL)
        time.sleep(0.1)
        q.proc.returncode = 0
        time.sleep(0.1)
        n = len(srv.commands("query-status"))
        time.sleep(0.2)
        assert len(srv.commands("query-status")) == n


def test_replies_are_never_swapped(monkeypatch):
    """Commands and pings run at once; each gets its own reply.

    Every 7th ping is answered late, after its timeout, so stale ping
    replies land in front of the caller's replies.
    """
    monkeypatch.setattr(qemu_mod, "KEEPALIVE_TIMEOUT", 0.01)
    late, count = [], [0]

    def reply(req):
        if req["execute"] == "query-status":
            count[0] += 1
            if count[0] % 7 == 0:
                late.append(req.get("id"))
                return False
            return None
        msgs = [{"return": "stale-ping", "id": late.pop()}] if late else []
        n = req.get("arguments", {}).get("n")
        return msgs + [{"return": {"n": n}, "id": req.get("id")}]
    with wired(reply) as (q, srv):
        q.start_keepalive(0.001)
        i = 0
        while count[0] < 50 and i < 5000:
            assert q.execute("echo", n=i) == {"n": i}
            time.sleep(0.0005)  # let the ping thread take the lock
            i += 1
        q.stop_keepalive()
        assert count[0] >= 50 and i >= 50, (count, i)
        assert q.keepalive_pings > 0


def test_late_command_reply_is_not_taken_by_a_ping():
    first = []

    def reply(req):
        if req["execute"] == "slow" and not first:
            first.append(req.get("id"))
            return False  # answered later, inside the next ping's exchange
        if req["execute"] == "query-status" and first:
            return [{"return": "late-slow", "id": first[0]}, default_reply(req)]
        return None
    with wired(reply) as (q, srv):
        with pytest.raises(socket.timeout):
            q.command("slow", timeout=0.05)
        q.start_keepalive(INTERVAL)
        time.sleep(0.1)
        assert q.execute("slow") == {}


def test_keepalive_default_follows_the_environment(monkeypatch):
    monkeypatch.delenv(qemu_mod.KEEPALIVE_ENV, raising=False)
    assert qemu_mod.keepalive_default() is True
    for off in ("0", "off", "OFF", "no", "false"):
        monkeypatch.setenv(qemu_mod.KEEPALIVE_ENV, off)
        assert qemu_mod.keepalive_default() is False
    monkeypatch.setenv(qemu_mod.KEEPALIVE_ENV, "1")
    assert qemu_mod.keepalive_default() is True


class BootQemu:
    """Stands in for Qemu inside boot(): records start_keepalive()."""
    started = []

    def __init__(self, *a, **k):
        self.proc = FakeProc()
        self.extra = []

    def _sock(self, name):
        return name

    def start(self):
        return self

    def execute(self, cmd, **kw):
        return {}

    def start_keepalive(self, interval):
        BootQemu.started.append(interval)

    def close(self):
        pass


@pytest.mark.parametrize("env, arg, on", [
    (None, None, True), ("0", None, False), ("off", True, True), (None, False, False),
])
def test_boot_switch(monkeypatch, env, arg, on):
    monkeypatch.setattr(boot_mod, "Qemu", BootQemu)
    monkeypatch.setattr(boot_mod, "boot_args", lambda *a: [])
    monkeypatch.setattr(boot_mod, "_connect_unix", lambda *a: socket.socketpair()[0])
    if env is None:
        monkeypatch.delenv(qemu_mod.KEEPALIVE_ENV, raising=False)
    else:
        monkeypatch.setenv(qemu_mod.KEEPALIVE_ENV, env)
    BootQemu.started = []
    with boot_mod.boot("x.img", "raspi0", keepalive=arg, elf=""):
        pass
    assert BootQemu.started == ([qemu_mod.KEEPALIVE_INTERVAL] if on else [])


# The state dump through a real Qemu client.

class DumpVm(Vm):
    def __init__(self, qemu):
        self.qemu = qemu
        self.elf = None
        self._bufs = [bytearray()]
        self._cond = threading.Condition()


def hmp_reply(fail=None):
    def reply(req):
        if req["execute"] != "human-monitor-command":
            return None
        cmd = req["arguments"]["command-line"]
        if fail and fail(cmd):
            return {"error": {"class": "GenericError", "desc": f"failed: {cmd}"},
                    "id": req.get("id")}
        if cmd == "info registers":
            text = "R13=00001000 R14=00008000 R15=00008004\nPSR=600001d3 -ZC- A S svc32\n"
            text += "".join(f"R{i:02d}=00000000\n" for i in range(13))
        else:
            addr = int(cmd.split()[-1], 16)
            text = f"{addr:016x}: 0x00000002 0x00001000 0x00000000 0x00000000\n" \
                   f"{addr + 16:016x}: 0x00001100 0x00000000 0x00000000\n"
        return {"return": text, "id": req.get("id")}
    return reply


def test_dump_stops_before_the_reads_and_resumes():
    with wired(hmp_reply()) as (q, srv):
        q.start_keepalive(INTERVAL)
        time.sleep(0.1)
        text = DumpVm(q).state_dump()
        cmds = [c for c in srv.commands() if c != "query-status"]
        assert cmds[0] == "stop" and cmds[-6] == "cont", cmds
        assert cmds[1:4] == ["human-monitor-command"] * 3  # ic, timer, cpu0 at stop
        assert q.vm_running
        assert "device registers: read with the VM stopped" in text
        assert "keep-alive: on" in text
        # No ping while the VM was stopped for the reads.
        log = srv.log
        t_stop = next(t for t, c, _ in log if c == "stop")
        t_cont = next(t for t, c, _ in log if c == "cont")
        assert not [c for t, c, _ in log if t_stop < t < t_cont and c == "query-status"]


@pytest.mark.parametrize("fail", ["xp", "info registers", "all"])
def test_dump_resumes_when_a_read_fails(fail):
    def failing(cmd):
        return fail == "all" or cmd.startswith(fail)
    with wired(hmp_reply(failing)) as (q, srv):
        text = DumpVm(q).state_dump()
        cmds = srv.commands()
        assert cmds.count("stop") == 1 and cmds.count("cont") == 1, cmds
        assert cmds.index("cont") > cmds.index("stop")
        assert q.vm_running
        assert "keep-alive: off" in text


def test_dump_leaves_a_stopped_vm_stopped():
    with wired(hmp_reply(), running=False) as (q, srv):
        text = DumpVm(q).state_dump()
        assert "stop" not in srv.commands() and "cont" not in srv.commands()
        assert not q.vm_running
    assert "device registers: read with the VM already stopped (no stop, no cont)" in text
    assert "cpu0 samples of the stopped VM (5, 100 ms apart):" in text
    assert "while running" not in text
    # Fixed registers on a stopped VM say nothing about execution.
    assert ("summary: guest executing: cannot be judged (the VM was stopped during the"
            " samples)") in text, text
    assert "guest executing: no" not in text


def test_dump_says_when_cont_fails():
    def reply(req):
        if req["execute"] == "cont":
            return {"error": {"class": "GenericError", "desc": "cont refused"},
                    "id": req.get("id")}
        return hmp_reply()(req)
    with wired(reply) as (q, srv):
        text = DumpVm(q).state_dump()
        assert srv.commands().count("stop") == 1 and srv.commands().count("cont") == 1
    assert "device registers: read with the VM stopped (QMP stop, then cont, which failed)" \
        in text
    assert ("cpu0 samples after a failed cont, the VM may still be stopped"
            " (5, 100 ms apart):") in text, text
    assert "while running" not in text
    assert ("summary: guest executing: cannot be judged (cont failed (QemuError: cont:"
            " cont refused), the VM may still be stopped)") in text, text
    assert "not collected: resume the VM (cont): QemuError: cont: cont refused" in text


def test_dump_normal_case_labels():
    with wired(hmp_reply()) as (q, srv):
        text = DumpVm(q).state_dump()
    assert "device registers: read with the VM stopped (QMP stop, then cont)" in text
    assert "cpu0 samples while running (5, 100 ms apart):" in text
    assert "summary: guest executing: no (pc and registers fixed over 5 samples)" in text


def test_dump_resumes_after_a_failed_stop():
    def reply(req):
        if req["execute"] == "stop":
            return {"error": {"class": "GenericError", "desc": "nope"}, "id": req.get("id")}
        return hmp_reply()(req)
    with wired(reply) as (q, srv):
        text = DumpVm(q).state_dump()
        assert srv.commands().count("cont") == 1
        assert "read with the VM running (QMP stop failed)" in text


def test_command_error_still_raises():
    def reply(req):
        return {"error": {"class": "GenericError", "desc": "bad"}, "id": req.get("id")}
    with wired(reply) as (q, srv):
        with pytest.raises(QemuError, match="^bogus: bad$"):
            q.execute("bogus")


def wait_until(cond, timeout=2.0):
    deadline = time.monotonic() + timeout
    while not cond():
        if time.monotonic() > deadline:
            return False
        time.sleep(0.01)
    return True


def test_stop_on_a_running_vm_reports_off():
    with wired(hmp_reply()) as (q, srv):
        q.start_keepalive(INTERVAL)
        assert wait_until(lambda: q.keepalive_pings > 0)
        q.stop_keepalive()
        assert q.vm_running and not q.keepalive_on
        text = DumpVm(q).state_dump()
    assert "keep-alive: off\n" in text, text


def test_closed_socket_reports_off_with_a_reason():
    a, b = socket.socketpair()
    q = Qemu("qemu-none", "raspi0", timeout=2.0)
    q._qmp = a
    a.settimeout(q.timeout)
    q.proc = FakeProc()
    q.vm_running = True
    srv = FakeServer(b)
    try:
        q.start_keepalive(INTERVAL)
        assert wait_until(lambda: q.keepalive_pings > 0)
        b.shutdown(socket.SHUT_RDWR)
        b.close()
        # A closed peer surfaces as EOF or a broken pipe, depending on timing.
        assert wait_until(lambda: not q.keepalive_on)
        text = DumpVm(q).state_dump()
        line = next(x for x in text.splitlines() if x.startswith("keep-alive:"))
        assert re.fullmatch(r"keep-alive: off \(stopped: (QemuError|\w*Error): .+\)", line), line
    finally:
        q.stop_keepalive()
        q.proc = None
        a.close()
        q.close()


def test_paused_vm_reports_on_paused():
    with wired(running=False) as (q, srv):
        q.start_keepalive(INTERVAL)
        assert q.keepalive_status() == "on (paused: VM stopped)"
        q.stop_keepalive()
        assert q.keepalive_status() == "off"


def test_start_stop_start_counts_again():
    with wired() as (q, srv):
        q.start_keepalive(INTERVAL)
        assert wait_until(lambda: q.keepalive_pings > 0)
        q.stop_keepalive()
        n = len(srv.commands("query-status"))
        time.sleep(0.1)
        assert len(srv.commands("query-status")) == n
        q.start_keepalive(INTERVAL)
        assert q.keepalive_on and q.keepalive_pings == 0
        assert wait_until(lambda: q.keepalive_pings >= 3)
        assert len(srv.commands("query-status")) > n


def test_double_start_and_double_stop_are_harmless():
    with wired() as (q, srv):
        q.start_keepalive(INTERVAL)
        first = q._ka_thread
        q.start_keepalive(INTERVAL)
        assert q._ka_thread is first
        assert len([t for t in threading.enumerate() if t.name == "qmp-keepalive"]) == 1
        q.stop_keepalive()
        q.stop_keepalive()
        assert not q.keepalive_on and q.keepalive_status() == "off"
        assert not first.is_alive()
