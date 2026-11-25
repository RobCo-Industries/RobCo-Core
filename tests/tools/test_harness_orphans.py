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


@pytest.fixture
def fake(tmp_path):
    """A fake QEMU binary. Kills every stand-in it started at the end."""
    script = tmp_path / "qemu-fake"
    script.write_text('#!/bin/sh\necho $$ >> "$(dirname "$0")/pids"\nexec sleep 600\n')
    script.chmod(0o755)
    yield script
    pids = tmp_path / "pids"
    if pids.exists():
        for pid in map(int, pids.read_text().split()):
            try:
                os.kill(pid, signal.SIGKILL)
            except OSError:
                pass


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
    time.sleep(600)
""")


@pytest.mark.parametrize("how", ["kill", "term", "exit"])
def test_qemu_dies_with_its_test_process(fake, how):
    code = CHILD.format(tests=TESTS, binary=str(fake), how=how)
    child = subprocess.Popen([sys.executable, "-c", code], stdout=subprocess.PIPE, text=True)
    try:
        assert child.stdout.readline().strip() == "started"
        (pid,) = stand_in_pids(fake)
        if how == "kill":
            child.kill()
        elif how == "term":
            child.terminate()
        child.wait(5)
        assert gone(pid), f"QEMU stand-in {pid} outlived its test process ({how})"
    finally:
        child.kill()
        child.wait()
        child.stdout.close()


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
