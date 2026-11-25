"""QEMU never outlives the test process that started it. No QEMU needed.

A stand-in script plays QEMU: it writes its pid next to itself and sleeps.
"""

import os
import signal
import subprocess
import sys
import tempfile
import textwrap
import time

import pytest

from harness import qemu as qemu_mod
from harness.qemu import Qemu, sweep_stale

TESTS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


@pytest.fixture(autouse=True, params=["process_group", "preexec_fn"])
def group_form(request, monkeypatch):
    """Run every test with each way of setting the process group.

    Python 3.11 and later use Popen(process_group=). Older ones fall back
    to preexec_fn. The environment variable reaches the child tests' VMs.
    """
    fallback = request.param == "preexec_fn"
    if not fallback and sys.version_info < (3, 11):
        pytest.skip("Popen(process_group=) needs Python 3.11")
    monkeypatch.setattr(qemu_mod, "USE_PROCESS_GROUP", not fallback)
    if fallback:
        monkeypatch.setenv(qemu_mod.PGID_FALLBACK_ENV, "1")
    else:
        monkeypatch.delenv(qemu_mod.PGID_FALLBACK_ENV, raising=False)
    return request.param


def write_fake(folder, body="exec sleep 600"):
    """A fake QEMU binary in `folder`: it records its pid, then runs `body`."""
    folder.mkdir(exist_ok=True)
    script = folder / "qemu-fake"
    script.write_text(f'#!/bin/sh\necho $$ >> "$(dirname "$0")/pids"\n{body}\n')
    script.chmod(0o755)
    return script


def kill_stand_ins(folder):
    pids = folder / "pids"
    if pids.exists():
        for pid in map(int, pids.read_text().split()):
            try:
                os.kill(pid, signal.SIGKILL)
            except OSError:
                pass


@pytest.fixture
def fake(tmp_path):
    """A fake QEMU binary. Kills every stand-in it started at the end."""
    yield write_fake(tmp_path)
    kill_stand_ins(tmp_path)


@pytest.fixture
def stubborn(tmp_path):
    """A fake QEMU that ignores SIGTERM, so close() waits for it."""
    yield write_fake(tmp_path / "stubborn", "trap '' TERM\nexec sleep 600")
    kill_stand_ins(tmp_path / "stubborn")


@pytest.fixture
def short(tmp_path):
    """A fake QEMU that exits by itself at once."""
    yield write_fake(tmp_path / "short", "exit 0")


def stand_in_pids(script, count=1, timeout=5.0):
    path = script.parent / "pids"
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            pids = [int(p) for p in path.read_text().split()]
            if len(pids) >= count:
                return pids
        time.sleep(0.01)
    raise AssertionError("the stand-in did not start")


def gone(pid, timeout=5.0):
    """True once `pid` names no process, zombie included."""
    deadline = time.monotonic() + timeout
    while True:
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return True
        if time.monotonic() >= deadline:
            return False
        time.sleep(0.02)


CHILD = textwrap.dedent("""
    import os, signal, sys, time
    sys.path.insert(0, {tests!r})
    from harness.qemu import Qemu, sweep_stale
    q = Qemu({binary!r}, "none", qmp=False).start()
    pids = os.path.join(os.path.dirname({binary!r}), "pids")
    while not os.path.exists(pids):
        time.sleep(0.01)
    print("started", flush=True)
    how = {how!r}
    if how == "exit":
        os._exit(0)
    if how == "close":
        print("closing", flush=True)
        q.close()
    if how == "own-exit":
        q.proc.wait()
        print("exited", flush=True)
    time.sleep(600)
""")


@pytest.mark.parametrize("how", ["kill", "term", "int", "hup", "exit"])
def test_qemu_dies_with_its_test_process(fake, how):
    # "int" and "hup": what a terminal sends the test process on Ctrl-C
    # and on close. QEMU's group is not the terminal's, so only the test
    # process gets them.
    code = CHILD.format(tests=TESTS, binary=str(fake), how=how)
    child = subprocess.Popen([sys.executable, "-c", code], stdout=subprocess.PIPE, text=True)
    try:
        assert child.stdout.readline().strip() == "started"
        (pid,) = stand_in_pids(fake)
        if how == "kill":
            child.kill()
        elif how == "term":
            child.terminate()
        elif how == "int":
            child.send_signal(signal.SIGINT)
        elif how == "hup":
            child.send_signal(signal.SIGHUP)
        child.wait(5)
        assert gone(pid), f"QEMU stand-in {pid} outlived its test process ({how})"
    finally:
        child.kill()
        child.wait()
        child.stdout.close()


def test_qemu_dies_when_test_process_dies_in_close(stubborn):
    # close() waits for a QEMU that ignores SIGTERM. The test process dies
    # then: the watcher must still be there to kill QEMU.
    code = CHILD.format(tests=TESTS, binary=str(stubborn), how="close")
    child = subprocess.Popen([sys.executable, "-c", code], stdout=subprocess.PIPE, text=True)
    try:
        assert child.stdout.readline().strip() == "started"
        (pid,) = stand_in_pids(stubborn)
        assert child.stdout.readline().strip() == "closing"
        time.sleep(0.5)  # close() now waits on the stand-in
        child.kill()
        child.wait(5)
        assert gone(pid, timeout=2.0), f"QEMU stand-in {pid} outlived a kill during close()"
    finally:
        child.kill()
        child.wait()
        child.stdout.close()


@pytest.fixture
def deaf(tmp_path):
    """A fake QEMU that ignores SIGINT, SIGHUP and SIGTERM."""
    yield write_fake(tmp_path / "deaf", "trap '' INT HUP TERM\nexec sleep 600")
    kill_stand_ins(tmp_path / "deaf")


EARLY_CLOSE = textwrap.dedent("""
    import os, sys
    sys.path.insert(0, {tests!r})
    from harness import qemu
    # Test-only: a watcher slow to set up widens the window before its trap.
    qemu.WATCHER = "sleep 0.2; " + qemu.WATCHER
    q = qemu.Qemu({binary!r}, "none", qmp=False).start()
    pids = os.path.join(os.path.dirname({binary!r}), "pids")
    while not os.path.exists(pids) or len(open(pids).read().split()) < {count}:
        pass  # the stand-in has recorded its pid: well inside the window
    print("closing", flush=True)
    q.close()
""")


def test_qemu_dies_when_test_process_dies_in_early_close(stubborn):
    # close() at once after start(), before a slow watcher could set any
    # trap. The test process dies while close() waits on a QEMU that
    # ignores SIGTERM. The watcher must still be there to kill QEMU.
    for i in range(30):
        code = EARLY_CLOSE.format(tests=TESTS, binary=str(stubborn), count=i + 1)
        child = subprocess.Popen([sys.executable, "-c", code], stdout=subprocess.PIPE, text=True)
        try:
            assert child.stdout.readline().strip() == "closing"
            pid = stand_in_pids(stubborn, count=i + 1)[i]
            time.sleep(0.3)  # close() now waits on the stand-in
            child.kill()
            child.wait(5)
            assert gone(pid, timeout=2.0), f"run {i}: QEMU stand-in {pid} outlived its test process"
        finally:
            child.kill()
            child.wait()
            child.stdout.close()


GROUP_SIGNAL = textwrap.dedent("""
    import os, sys, time
    sys.path.insert(0, {tests!r})
    from harness.qemu import Qemu
    q = Qemu({binary!r}, "none", qmp=False).start()
    print(q._watcher.pid, flush=True)
    time.sleep(600)
""")


@pytest.mark.parametrize("sig", [signal.SIGTERM, signal.SIGINT, signal.SIGHUP])
def test_watcher_survives_a_signal_to_its_group(deaf, sig):
    # Someone signals the whole VM group. QEMU ignores it here. The
    # watcher must survive and still kill QEMU when the test process dies.
    code = GROUP_SIGNAL.format(tests=TESTS, binary=str(deaf))
    child = subprocess.Popen([sys.executable, "-c", code], stdout=subprocess.PIPE, text=True)
    try:
        watcher = int(child.stdout.readline())
        (pid,) = stand_in_pids(deaf)
        os.killpg(watcher, sig)
        time.sleep(0.3)
        os.kill(watcher, 0)  # raises if the watcher died
        assert not gone(pid, timeout=0.0)
        child.kill()
        child.wait(5)
        assert gone(pid, timeout=2.0), f"QEMU stand-in {pid} outlived its test process"
    finally:
        child.kill()
        child.wait()
        child.stdout.close()


def test_dead_test_process_signals_no_bystander(short, tmp_path):
    # QEMU exits by itself, then the test process dies. The watcher kills
    # its own group only, so a process outside it lives on.
    bystander = subprocess.Popen(["sleep", "600"], start_new_session=True)
    code = CHILD.format(tests=TESTS, binary=str(short), how="own-exit")
    child = subprocess.Popen([sys.executable, "-c", code], stdout=subprocess.PIPE, text=True)
    try:
        assert child.stdout.readline().strip() == "started"
        assert child.stdout.readline().strip() == "exited"
        child.kill()
        child.wait(5)
        time.sleep(0.5)  # the watcher acts on end of file
        assert bystander.poll() is None, "the watcher signalled a bystander"
    finally:
        child.kill()
        child.wait()
        child.stdout.close()
        bystander.kill()
        bystander.wait()


def test_close_leaves_no_process(fake):
    q = Qemu(str(fake), "none", qmp=False).start()
    (pid,) = stand_in_pids(fake)
    watcher = q._watcher.pid
    q.close()
    # close() reaped both: no process and no zombie under our pid.
    assert gone(pid, timeout=0.0)
    assert gone(watcher, timeout=0.0)
    assert not os.path.exists(q.tmpdir)


def open_fds():
    return len(os.listdir("/dev/fd"))


def test_start_close_leaks_no_fd(fake):
    Qemu(str(fake), "none", qmp=False).start().close()  # warm-up
    before = open_fds()
    for _ in range(50):
        Qemu(str(fake), "none", qmp=False).start().close()
    assert open_fds() == before
    pids = fake.parent / "pids"
    for pid in map(int, pids.read_text().split() if pids.exists() else []):
        assert gone(pid, timeout=1.0)


def dead_pid():
    p = subprocess.Popen(["true"])
    p.wait()
    return p.pid


def stale_vm(tmp, started_offset=0.0, owner=None):
    """A sleeping process and its pidfile, as a dead run leaves them."""
    proc = subprocess.Popen(["sleep", "600"], start_new_session=True)
    folder = tempfile.mkdtemp(prefix=qemu_mod.TMP_PREFIX, dir=tmp)
    with open(os.path.join(folder, qemu_mod.PIDFILE), "w") as f:
        f.write(f"{proc.pid} {time.time() + started_offset:.3f} "
                f"{owner or dead_pid()} {proc.pid}\n")
    return proc, folder


def test_sweep_kills_only_its_own(tmp_path):
    if not ps_works():
        pytest.skip("cannot read a process start time here: ps may not run")
    own, own_dir = stale_vm(tmp_path)
    # Same pidfile shape, wrong start time: the pid is not the recorded VM.
    other, other_dir = stale_vm(tmp_path, started_offset=-3600)
    # Its owner still runs: another session's VM.
    live, live_dir = stale_vm(tmp_path, owner=os.getppid())
    try:
        assert sweep_stale(str(tmp_path)) == [own.pid]
        assert own.wait(5) == -signal.SIGKILL
        assert not os.path.exists(own_dir)
        assert other.poll() is None and os.path.exists(other_dir)
        assert live.poll() is None and os.path.exists(live_dir)
    finally:
        for p in (own, other, live):
            p.kill()
            p.wait()


def ps_works():
    """True when this process may run ps and read a start time from it."""
    return qemu_mod.process_start(os.getpid()) is not None


# -- the sweep never stops a session -------------------------------------

def ps_raises(pid):
    raise PermissionError(1, "Operation not permitted", "ps")


def ps_fails(pid):
    return subprocess.CompletedProcess(["ps"], 1, "", "ps: not allowed\n")


def ps_garbage(pid):
    return subprocess.CompletedProcess(["ps"], 0, "\x00not a date at all\n", "")


def ps_hangs(pid):
    # The real launcher with a hanging command and the real timeout.
    return subprocess.run(["sleep", "30"], capture_output=True, text=True,
                          timeout=qemu_mod.PS_TIMEOUT)


@pytest.fixture
def no_proc(monkeypatch, tmp_path):
    """Take the ps path on every system."""
    monkeypatch.setattr(qemu_mod, "PROC", str(tmp_path / "no-proc"))


@pytest.mark.parametrize("launcher", [ps_raises, ps_fails, ps_garbage, ps_hangs])
def test_process_start_is_none_when_ps_fails(monkeypatch, no_proc, launcher):
    monkeypatch.setattr(qemu_mod, "_run_ps", launcher)
    t = time.monotonic()
    assert qemu_mod.process_start(os.getpid()) is None
    assert time.monotonic() - t < qemu_mod.PS_TIMEOUT + 2


@pytest.mark.parametrize("pid", ["x", None, 0, 1, -1, 10**30])
def test_process_start_is_none_for_a_bad_pid(no_proc, pid):
    assert qemu_mod.process_start(pid) is None


def test_process_start_is_none_for_unreadable_proc(monkeypatch, tmp_path):
    proc = tmp_path / "proc"
    (proc / "self").mkdir(parents=True)
    (proc / str(os.getpid())).mkdir()
    (proc / str(os.getpid()) / "stat").write_text("garbage")
    monkeypatch.setattr(qemu_mod, "PROC", str(proc))
    assert qemu_mod.process_start(os.getpid()) is None  # unparsable
    assert qemu_mod.process_start(os.getpid() + 1) is None  # gone


class _NoKillOs:
    """The os module as the harness sees it, with kill and killpg recorded."""

    def __init__(self, sent):
        self._sent = sent

    def __getattr__(self, name):
        return getattr(os, name)

    def kill(self, pid, sig):
        self._sent.append((pid, sig))
        if sig != 0 or pid <= 1:
            raise AssertionError(f"the sweep sent signal {sig} to pid {pid}")
        return os.kill(pid, 0)

    def killpg(self, pgid, sig):
        self._sent.append((-pgid, sig))
        raise AssertionError(f"the sweep signalled group {pgid}")


@pytest.fixture
def no_kill(monkeypatch):
    """Record every signal the sweep tries to send, and send none."""
    sent = []
    monkeypatch.setattr(qemu_mod, "os", _NoKillOs(sent))
    return sent


def test_sweep_spares_a_process_it_cannot_read(monkeypatch, tmp_path, no_kill, capsys):
    monkeypatch.setattr(qemu_mod, "process_start", lambda pid: None)
    proc, folder = stale_vm(tmp_path)
    try:
        assert sweep_stale(str(tmp_path)) == []
        assert proc.poll() is None
        assert os.path.exists(os.path.join(folder, qemu_mod.PIDFILE))
        assert all(sig == 0 for _, sig in no_kill)
        assert capsys.readouterr().err.count("could not check") == 1
    finally:
        proc.kill()
        proc.wait()


def test_sweep_spares_all_when_ps_may_not_run(monkeypatch, no_proc, tmp_path, no_kill):
    monkeypatch.setattr(qemu_mod, "_run_ps", ps_raises)
    procs = [stale_vm(tmp_path) for _ in range(3)]
    try:
        assert sweep_stale(str(tmp_path)) == []
        for proc, folder in procs:
            assert proc.poll() is None and os.path.exists(folder)
    finally:
        for proc, _ in procs:
            proc.kill()
            proc.wait()


BROKEN = [b"", b"\x00\xff\xfe\x80garbage", b"123 456.0", b"0 1.0 0 0",
          b"1 1.0 1 1", b"-1 1.0 -1 -1", b"-1 1.0 999999 -1",
          b"0 1.0 999999 0", b"12345 nan 999999 12345", b"12345 inf 999999 12345"]


@pytest.mark.parametrize("content", BROKEN)
def test_sweep_ignores_a_broken_pidfile(monkeypatch, tmp_path, no_kill, content):
    monkeypatch.setattr(qemu_mod, "process_start", lambda pid: 0.0)  # anything "proven"
    folder = tempfile.mkdtemp(prefix=qemu_mod.TMP_PREFIX, dir=tmp_path)
    with open(os.path.join(folder, qemu_mod.PIDFILE), "wb") as f:
        f.write(content)
    assert sweep_stale(str(tmp_path)) == []
    assert all(sig == 0 and pid > 1 for pid, sig in no_kill)
    assert os.path.exists(os.path.join(folder, qemu_mod.PIDFILE))


def test_sweep_never_raises(monkeypatch, tmp_path):
    def boom(pid):
        raise RuntimeError("boom")
    monkeypatch.setattr(qemu_mod, "process_start", boom)
    _, folder = stale_vm(tmp_path, owner=999999)
    try:
        assert sweep_stale(str(tmp_path)) == []
        assert os.path.exists(folder)
    finally:
        with open(os.path.join(folder, qemu_mod.PIDFILE)) as f:
            os.kill(int(f.read().split()[0]), signal.SIGKILL)


def test_session_starts_when_the_sweep_raises(monkeypatch, capsys):
    from harness import plugin

    def boom():
        raise PermissionError("ps not allowed")
    monkeypatch.setattr(plugin, "sweep_stale", boom)
    plugin.pytest_sessionstart(None)
    assert "skipped the QEMU sweep" in capsys.readouterr().err


def test_vm_without_a_watcher_does_not_start(monkeypatch, fake):
    real = subprocess.Popen

    def popen(cmd, *a, **k):
        if cmd[0] == "/bin/sh":
            raise PermissionError(1, "Operation not permitted", "/bin/sh")
        return real(cmd, *a, **k)
    monkeypatch.setattr(qemu_mod.subprocess, "Popen", popen)
    q = Qemu(str(fake), "none", qmp=False)
    with pytest.raises(qemu_mod.QemuError, match="watcher"):
        q.start()
    assert not (fake.parent / "pids").exists(), "QEMU started without a watcher"
    assert not os.path.exists(q.tmpdir)


def test_group_form_matches_the_python(group_form):
    assert qemu_mod._group_kwargs(0).keys() == {group_form}


def test_vm_without_a_watcher_group_does_not_start(monkeypatch, fake):
    # The watcher dies and is reaped before QEMU starts: its group is
    # gone, setpgid() fails in the child and QEMU never runs.
    real = Qemu._start_watcher

    def start_then_lose(self):
        real(self)
        self._watcher.kill()
        self._watcher.wait()
    monkeypatch.setattr(Qemu, "_start_watcher", start_then_lose)
    before = open_fds()
    q = Qemu(str(fake), "none", qmp=False)
    with pytest.raises(qemu_mod.QemuError, match="cannot start QEMU"):
        q.start()
    assert q.proc is None or q.proc.poll() is not None
    assert not (fake.parent / "pids").exists(), "QEMU ran outside the watcher's group"
    assert not os.path.exists(q.tmpdir)
    assert open_fds() == before


def test_vm_starts_when_the_pidfile_cannot_be_written(monkeypatch, fake, capsys):
    def replace(*a):
        raise PermissionError(13, "Permission denied")
    monkeypatch.setattr(qemu_mod.os, "replace", replace)
    q = Qemu(str(fake), "none", qmp=False).start()
    try:
        assert "cannot write" in capsys.readouterr().err
    finally:
        q.close()
