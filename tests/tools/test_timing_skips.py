"""tests/target/conftest.py fails a session where one board has too many host timing skips. No QEMU needed.

Each test runs pytest in a subprocess on a temporary project that uses
the target conftest and tests that skip on purpose. A skip counts
against its test's board parameter.
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


# Stands in for the plugin's board fixture, which skips boards QEMU lacks.
BOARD = "@pytest.fixture\ndef board(request):\n    return request.param\n"


def timing(name):
    return f"def test_{name}():\n    hosttiming.skip('a tick gap of 9.0 ms {name}')\n"


def on_board(name, skip_on):
    """A board test that skips for host timing on the boards in skip_on."""
    return (f"def test_{name}(board):\n    if board in {skip_on!r}:\n"
            f"        hosttiming.skip('a tick gap of 9.0 ms {name}')\n")


def test_no_timing_skip_passes(tmp_path):
    r = run(tmp_path, "def test_ok():\n    pass\n", "--board", "raspi0")
    assert r.returncode == 0, r.stdout


def test_one_on_one_board_passes(tmp_path):
    r = run(tmp_path, BOARD + on_board("a", ["raspi0"]), "--board", "raspi0")
    assert r.returncode == 0, r.stdout
    assert "raspi0: test_skips.py::test_a[raspi0]: Skipped: host timing: a tick gap of 9.0 ms a" in r.stdout
    assert "1 host timing skips (raspi0 1), limit 1 per board: within the limit" in r.stdout


def test_two_on_one_board_of_two_fails(tmp_path):
    body = BOARD + on_board("a", ["raspi0"]) + on_board("b", ["raspi0"])
    r = run(tmp_path, body, "--board", "raspi0", "--board", "raspi2b")
    assert r.returncode == 1, r.stdout
    assert "raspi0: test_skips.py::test_a[raspi0]: Skipped: host timing: a tick gap of 9.0 ms a" in r.stdout
    assert "raspi0: test_skips.py::test_b[raspi0]: Skipped: host timing: a tick gap of 9.0 ms b" in r.stdout
    assert "2 host timing skips (raspi0 2), limit 1 per board: FAILED, over the limit: raspi0" in r.stdout


def test_one_on_each_of_two_boards_passes(tmp_path):
    body = BOARD + on_board("a", ["raspi0"]) + on_board("b", ["raspi2b"])
    r = run(tmp_path, body, "--board", "raspi0", "--board", "raspi2b")
    assert r.returncode == 0, r.stdout
    assert "2 host timing skips (raspi0 1, raspi2b 1), limit 1 per board: within the limit" in r.stdout


def test_tests_without_a_board_share_one_bucket(tmp_path):
    r = run(tmp_path, timing("a"), "--board", "raspi0", "--board", "raspi2b")
    assert r.returncode == 0, r.stdout
    assert "(no board): test_skips.py::test_a: Skipped" in r.stdout
    r = run(tmp_path, timing("a") + timing("b"), "--board", "raspi0", "--board", "raspi2b")
    assert r.returncode == 1, r.stdout
    assert "FAILED, over the limit: (no board)" in r.stdout


def test_other_skips_are_not_counted(tmp_path):
    body = timing("a") + "def test_c():\n    pytest.skip('qemu-system-arm -M raspi0 not available')\n"
    r = run(tmp_path, body, "--board", "raspi0")
    assert r.returncode == 0, r.stdout
    assert "1 host timing skips" in r.stdout


def test_option_sets_the_limit_per_board(tmp_path):
    two = BOARD + on_board("a", ["raspi0", "raspi2b"]) + on_board("b", ["raspi0", "raspi2b"])
    boards = ["--board", "raspi0", "--board", "raspi2b"]
    r = run(tmp_path, two, *boards, "--timing-skips-max=2")
    assert r.returncode == 0, r.stdout
    assert "limit 2 per board: within the limit" in r.stdout
    three = two + on_board("c", ["raspi2b"])
    r = run(tmp_path, three, *boards, "--timing-skips-max=2")
    assert r.returncode == 1, r.stdout
    assert "FAILED, over the limit: raspi2b" in r.stdout
    assert run(tmp_path, timing("a"), "--board", "raspi0", "--timing-skips-max=0").returncode == 1
