"""tests/target/conftest.py fails a session with too many host timing skips. No QEMU needed.

Each test runs pytest in a subprocess on a temporary project that uses
the target conftest and tests that skip on purpose.
"""

import os
import shutil
import subprocess
import sys

TESTS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, TESTS)

from harness.env import minimal_env  # noqa: E402

PLUGIN = f"""
import sys
sys.path.insert(0, {TESTS!r})
pytest_plugins = ["harness.plugin"]
"""


def run(tmp_path, body, *args):
    shutil.copy(os.path.join(TESTS, "target", "conftest.py"), tmp_path / "conftest.py")
    (tmp_path / "test_skips.py").write_text("import pytest\nfrom harness import hosttiming\n" + body)
    (tmp_path / "pytest.ini").write_text("[pytest]\n")
    # harness.plugin goes in through -p rootplug; the target conftest is the directory's own.
    (tmp_path / "rootplug.py").write_text(PLUGIN)
    env = minimal_env()
    env["PYTHONPATH"] = str(tmp_path)
    return subprocess.run(
        [sys.executable, "-m", "pytest", "-p", "rootplug", "-q", "-rs", "-p", "no:cacheprovider",
         str(tmp_path), *args],
        cwd=tmp_path, env=env, capture_output=True, text=True, timeout=60)


def timing(name):
    return f"def test_{name}():\n    hosttiming.skip('a tick gap of 9.0 ms {name}')\n"


def test_no_timing_skip_passes(tmp_path):
    r = run(tmp_path, "def test_ok():\n    pass\n", "--board", "raspi0")
    assert r.returncode == 0, r.stdout


def test_one_on_one_board_passes(tmp_path):
    r = run(tmp_path, timing("a"), "--board", "raspi0")
    assert r.returncode == 0, r.stdout
    assert "1 host timing skips, limit 1: within the limit" in r.stdout


def test_two_on_one_board_fails_and_names_both(tmp_path):
    r = run(tmp_path, timing("a") + timing("b"), "--board", "raspi0")
    assert r.returncode == 1, r.stdout
    assert "test_skips.py::test_a: Skipped: host timing: a tick gap of 9.0 ms a" in r.stdout
    assert "test_skips.py::test_b: Skipped: host timing: a tick gap of 9.0 ms b" in r.stdout
    assert "2 host timing skips, limit 1: FAILED, over the limit" in r.stdout


def test_two_on_two_boards_pass(tmp_path):
    r = run(tmp_path, timing("a") + timing("b"), "--board", "raspi0", "--board", "raspi2b")
    assert r.returncode == 0, r.stdout


def test_other_skips_are_not_counted(tmp_path):
    body = timing("a") + "def test_c():\n    pytest.skip('qemu-system-arm -M raspi0 not available')\n"
    r = run(tmp_path, body, "--board", "raspi0")
    assert r.returncode == 0, r.stdout
    assert "1 host timing skips" in r.stdout


def test_option_overrides_the_limit(tmp_path):
    body = timing("a") + timing("b")
    assert run(tmp_path, body, "--board", "raspi0", "--timing-skips-max=2").returncode == 0
    assert run(tmp_path, timing("a"), "--board", "raspi0", "--timing-skips-max=0").returncode == 1
