"""harness.plugin from a project outside the repository, and harness.run.

The plugin test runs pytest in a subprocess on a copy of
tests/fixtures/sample-project in a temporary directory. The runner test
starts python3 -m harness.run with stdin from a pipe.
"""

import os
import shutil
import subprocess
import sys
import time

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TESTS = os.path.join(ROOT, "tests")
SAMPLE = os.path.join(ROOT, "tests", "fixtures", "sample-project")
sys.path.insert(0, TESTS)

from harness.qemu import have_machine  # noqa: E402

need_target = pytest.mark.skipif(
    shutil.which("arm-none-eabi-gcc") is None or not have_machine("qemu-system-arm", "raspi0"),
    reason="needs arm-none-eabi-gcc and qemu-system-arm -M raspi0")


@pytest.fixture
def project(tmp_path_factory):
    dest = tmp_path_factory.mktemp("sibling") / "sample-project"
    shutil.copytree(SAMPLE, dest, ignore=shutil.ignore_patterns("build", "__pycache__"))
    return dest


def pytest_in(project, *args):
    env = dict(os.environ, ROBCO_CORE=ROOT)
    return subprocess.run([sys.executable, "-m", "pytest", "-p", "no:cacheprovider",
                           str(project / "tests"), *args],
                          cwd=project, env=env, capture_output=True, text=True)


@need_target
def test_plugin_from_a_project(project):
    r = pytest_in(project, "-v", "--board", "raspi0")
    assert r.returncode == 0, r.stdout + r.stderr
    assert "test_ready[raspi0] PASSED" in r.stdout
    assert (project / "build" / "sample.img").exists()


def test_plugin_default_boards_and_ini(project):
    r = pytest_in(project, "--collect-only", "-q")
    assert "test_ready[raspi0]" in r.stdout and "test_ready[raspi2b]" in r.stdout, r.stdout
    (project / "pytest.ini").write_text("[pytest]\nrobco_boards = raspi3b\n")
    r = pytest_in(project, "--collect-only", "-q")
    assert "test_ready[raspi3b]" in r.stdout and "raspi0" not in r.stdout, r.stdout
    r = pytest_in(project, "--collect-only", "-q", "--board", "all")
    assert r.stdout.count("test_ready[") == 4, r.stdout


def test_plugin_rejects_unknown_board(project):
    r = pytest_in(project, "--collect-only", "-q", "--board", "raspi9")
    assert r.returncode != 0
    assert "unknown --board ['raspi9']" in r.stdout + r.stderr


def read_until(stream, needle, timeout):
    """Reads a non-blocking pipe until needle shows up. Returns what was read."""
    buf = b""
    deadline = time.monotonic() + timeout
    while needle not in buf and time.monotonic() < deadline:
        try:
            data = os.read(stream.fileno(), 4096)
        except BlockingIOError:
            data = None
        if data == b"":
            break
        if data:
            buf += data
        else:
            time.sleep(0.05)
    return buf


def process_gone(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return True
    return False


@need_target
def test_runner_boots_and_quits():
    subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "examples", "hello"), "CI=1"],
                   check=True, stdout=subprocess.DEVNULL)
    image = os.path.join(ROOT, "examples", "hello", "build", "hello.img")
    env = dict(os.environ, PYTHONPATH=TESTS)
    p = subprocess.Popen([sys.executable, "-m", "harness.run", image, "--board", "raspi0"],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         env=env)
    try:
        os.set_blocking(p.stdout.fileno(), False)
        out = read_until(p.stdout, b"@RC stage=hello ev=ready", 60)
        assert b"@RC stage=hello ev=boot" in out, out
        assert b"@RC stage=hello ev=ready" in out, out
        p.stdin.write(b"abc\x01x")
        p.stdin.flush()
        assert p.wait(30) == 0
    finally:
        if p.poll() is None:
            p.kill()
            p.wait()
    err = p.stderr.read().decode()
    line = [x for x in err.splitlines() if "qemu pid" in x]
    assert line, err
    pid = int(line[0].split("qemu pid ")[1].split(",")[0])
    assert process_gone(pid), f"QEMU {pid} still runs"


def test_runner_usage():
    env = dict(os.environ, PYTHONPATH=TESTS)
    r = subprocess.run([sys.executable, "-m", "harness.run", "x.img", "--board", "raspi0",
                        "--serial2", "udp:1"], env=env, capture_output=True, text=True)
    assert r.returncode == 2 and "--serial2 takes none, pty or tcp:PORT" in r.stderr
    from harness import run
    cmd = run.command("x.img", "raspi2b", serial2="tcp:4555", sd="sd.img")
    assert cmd[:2] == ["qemu-system-arm", "-M"]
    assert ["-serial", "mon:stdio"] == cmd[6:8]
    assert "tcp:127.0.0.1:4555,server=on,wait=off" in cmd
    assert "if=sd,format=raw,file=sd.img" in cmd
