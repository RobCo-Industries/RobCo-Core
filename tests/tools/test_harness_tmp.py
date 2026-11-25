"""harness.plugin removes a passing test's tmp_path when the test ends.

Each test runs a small pytest session in a subprocess with the plugin
loaded. Its tests run in file order: one passes, one fails, and a third
records whether the first one's folder still exists. The outer test
reads what they wrote.
"""

import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TESTS = os.path.join(ROOT, "tests")
sys.path.insert(0, TESTS)

from harness.env import minimal_env  # noqa: E402

CONFTEST = f"""
import sys
sys.path.insert(0, {TESTS!r})
pytest_plugins = ["harness.plugin"]
"""

INNER = """
import os

OUT = os.environ["TMP_TEST_OUT"]


def note(name, value):
    with open(os.path.join(OUT, name), "w") as f:
        f.write(str(value))


def test_a_pass(tmp_path):
    (tmp_path / "card.img").write_bytes(bytes(4096))
    note("pass_dir", tmp_path)


def test_b_fail(tmp_path):
    (tmp_path / "card.img").write_bytes(bytes(4096))
    note("fail_dir", tmp_path)
    assert False, "deliberate"


def test_c_after(tmp_path):
    with open(os.path.join(OUT, "pass_dir")) as f:
        note("pass_dir_after_test", os.path.exists(os.path.join(f.read(), "card.img")))
"""


def run_inner(tmp_path, ini=None, **env):
    proj, out = tmp_path / "proj", tmp_path / "out"
    proj.mkdir()
    out.mkdir()
    (proj / "conftest.py").write_text(CONFTEST)
    (proj / "test_inner.py").write_text(INNER)
    if ini:
        (proj / "pytest.ini").write_text(ini)
    r = subprocess.run(
        [sys.executable, "-m", "pytest", "-q", "-p", "no:cacheprovider",
         "--basetemp", str(tmp_path / "base"), "test_inner.py"],
        cwd=proj, env=minimal_env(TMP_TEST_OUT=str(out), **env),
        capture_output=True, text=True)
    assert "1 failed, 2 passed" in r.stdout, r.stdout + r.stderr

    def read(name):
        return (out / name).read_text()

    return {
        "pass_gone_during_run": read("pass_dir_after_test") == "False",
        "pass_kept": os.path.exists(os.path.join(read("pass_dir"), "card.img")),
        "fail_kept": os.path.exists(os.path.join(read("fail_dir"), "card.img")),
    }


def test_passing_goes_failing_stays(tmp_path):
    got = run_inner(tmp_path)
    assert got == {"pass_gone_during_run": True, "pass_kept": False, "fail_kept": True}


def test_keep_switch_keeps_both(tmp_path):
    got = run_inner(tmp_path, ROBCO_KEEP_TMP="1")
    assert got == {"pass_gone_during_run": False, "pass_kept": True, "fail_kept": True}


def test_keep_switch_zero_is_off(tmp_path):
    got = run_inner(tmp_path, ROBCO_KEEP_TMP="0")
    assert got["pass_kept"] is False and got["fail_kept"] is True


def test_project_policy_wins(tmp_path):
    got = run_inner(tmp_path, ini="[pytest]\ntmp_path_retention_policy = all\n")
    assert got["pass_kept"] is True and got["fail_kept"] is True
