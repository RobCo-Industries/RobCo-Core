"""No test value may hold a copy of the host environment.

pytest prints arguments and locals of failing frames. A full copy of
os.environ in a test value would print every secret of the host. Tests build
subprocess environments with harness.env.minimal_env instead.
"""

import os
import re
import subprocess
import sys
import textwrap

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TESTS = os.path.join(ROOT, "tests")
HELPER = os.path.join(TESTS, "harness", "env.py")
THIS = os.path.abspath(__file__)

FULL_COPY = re.compile(
    r"dict\(\s*os\.environ"
    r"|os\.environ\.copy\("
    r"|\*\*\s*os\.environ\b"
    r"|env\s*=\s*os\.environ\b"
    r"|=\s*os\.environ\s*$"
    r"|copy\.(deep)?copy\(\s*os\.environ"
    r"|(dict|Env)\(\s*os\.environ\.items\(",
    re.M)

CANARY = "canary-12345"


def sources():
    for top in ("tests", "tools"):
        for d, dirs, files in os.walk(os.path.join(ROOT, top)):
            dirs[:] = [x for x in dirs if x not in ("__pycache__", "build", ".venv")]
            for f in files:
                if f.endswith((".py", ".sh")):
                    yield os.path.join(d, f)


def test_no_full_environment_copy():
    bad = []
    for path in sources():
        if path in (HELPER, THIS):
            continue
        with open(path, encoding="utf-8", errors="replace") as f:
            for n, line in enumerate(f, 1):
                if FULL_COPY.search(line):
                    bad.append(f"{os.path.relpath(path, ROOT)}:{n}")
    assert not bad, "copy of os.environ outside tests/harness/env.py: " + ", ".join(bad)


def test_pattern_catches_the_old_forms():
    for line in ('e = dict(os.environ, A="1")', "e = os.environ.copy()",
                 "e = {**os.environ}", "run(x, env=os.environ)", "e = os.environ"):
        assert FULL_COPY.search(line), line
    assert not FULL_COPY.search('os.environ.get("ROBCO_NET_TESTS")')


FAILING = """
import os
import sys
sys.path.insert(0, {tests!r})
from harness.env import minimal_env


def build():
    return {build}


def launch(cmd, env):
    assert cmd == "never", "fails on purpose"


def test_fails():
    env = build()
    launch("echo", env)
"""


def child_output(tmp_path, build, *args):
    (tmp_path / "test_throwaway.py").write_text(
        textwrap.dedent(FAILING.format(tests=TESTS, build=build)))
    # pytest sorts dict keys and cuts long reprs: a name that sorts first shows
    # even in the default style, like AWS_ or API_ secrets on a real host.
    env = {"A_ROBCO_LEAK_CANARY": CANARY, "PATH": os.environ.get("PATH", os.defpath)}
    r = subprocess.run([sys.executable, "-m", "pytest", "-p", "no:cacheprovider",
                        *args, str(tmp_path)],
                       cwd=tmp_path, env=env, capture_output=True, text=True)
    assert r.returncode == 1, "the throwaway test must fail"
    return r.stdout + r.stderr


# -vv turns off the cut of long reprs, so a full copy prints every value.
STYLES = ((), ("-vv",), ("--tb=long", "-vv"), ("--showlocals",), ("--showlocals", "-vv"))


def test_helper_does_not_leak(tmp_path):
    for args in STYLES:
        out = child_output(tmp_path, 'minimal_env(EXTRA="x")', *args)
        assert "fails on purpose" in out
        leaks = out.count(CANARY)
        assert leaks == 0, f"canary leaked {leaks} times with {args}"


def test_the_old_way_leaks(tmp_path):
    # Proves the check above can see a leak: the full copy prints the canary.
    old = "dict(os." + "environ)"
    counts = {args: child_output(tmp_path, old, *args).count(CANARY) for args in STYLES}
    assert counts[()] > 0 and counts[("--showlocals", "-vv")] > 0, counts
