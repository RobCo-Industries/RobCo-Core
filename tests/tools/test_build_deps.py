"""An example tracks headers whatever the spelling of BUILD.

The harness passes an absolute BUILD, `make` alone a relative one. gcc
writes each .d file with the object named as the compile rule spelled it.
When the spellings differ, make never matches the .d to the target, so a
header change does not rebuild the object and an image links a stale one.
mk/robco.mk gives BUILD one spelling. mk/host.mk builds from it.
"""
import os
import shutil
import subprocess
import time

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXAMPLE = os.path.join(ROOT, "examples", "hello")
REL = "build/deptest"
OBJ = "obj/main.o"
SRC = os.path.join(EXAMPLE, "main.c")
HDR = os.path.join(EXAMPLE, "robco_config.h")


def make(*args):
    env = {"PATH": os.environ.get("PATH", ""), "HOME": os.environ.get("HOME", "")}
    return subprocess.run(["make", "-s", "-C", EXAMPLE, "CI=1", *args], env=env,
                          stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
                          timeout=120)


@pytest.fixture
def deptest():
    if shutil.which("arm-none-eabi-gcc") is None:
        pytest.skip("no cross compiler")
    out = os.path.join(EXAMPLE, REL)
    shutil.rmtree(out, ignore_errors=True)
    hdr_time = os.stat(HDR).st_mtime
    yield out
    shutil.rmtree(out, ignore_errors=True)
    os.utime(HDR, (hdr_time, hdr_time))


@pytest.mark.parametrize("first, second", [("rel", "abs"), ("abs", "rel")])
def test_header_change_rebuilds_object(deptest, first, second):
    spell = {"rel": REL, "abs": deptest}
    r = make(f"BUILD={spell[first]}", f"{REL}/{OBJ}")
    assert r.returncode == 0, r.stderr
    obj = os.path.join(deptest, OBJ)
    with open(obj[:-2] + ".d") as f:
        assert f.readline().startswith(f"{REL}/{OBJ}:")
    # The object is newer than its source and older than a header it
    # includes, as after an edit of that header.
    t = max(os.stat(SRC).st_mtime, time.time() - 10)
    os.utime(obj, (t, t))
    now = max(time.time(), t + 1)
    os.utime(HDR, (now, now))
    # make -q: 0 when the target is up to date, 1 when it would rebuild.
    r = make("-q", f"BUILD={spell[second]}", f"{REL}/{OBJ}")
    assert r.returncode == 1, "the header change did not reach the object"


@pytest.mark.parametrize("var", ["BUILD=build", "BUILD={root}/build",
                                 "HOST_BUILD=build/host", "HOST_BUILD={root}/build/host"])
def test_host_build_has_one_spelling(var):
    out = subprocess.run(["make", "-pn", "-C", ROOT, var.format(root=ROOT), "venv"],
                         capture_output=True, text=True, timeout=60).stdout
    line = next(x for x in out.splitlines() if x.startswith("HOST_BUILD "))
    assert line.split("=", 1)[1].strip() == "build/host", line
