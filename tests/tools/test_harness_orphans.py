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
