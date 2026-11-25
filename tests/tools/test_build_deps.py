"""An example tracks headers whatever the spelling of BUILD.

The harness passes an absolute BUILD, `make` alone a relative one. gcc
writes each .d file with the object named as the compile rule spelled it.
When the spellings differ, make never matches the .d to the target, so a
header change does not rebuild the object and an image links a stale one.
mk/robco.mk keeps the caller's spelling and records it in $(BUILD)/.spelling.
A new spelling rebuilds every object, which rewrites the .d files.
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
    r = make(f"BUILD={spell[first]}", f"{spell[first]}/{OBJ}")
    assert r.returncode == 0, r.stderr
    obj = os.path.join(deptest, OBJ)
    with open(obj[:-2] + ".d") as f:
        assert f.readline().startswith(f"{spell[first]}/{OBJ}:")
    # A new spelling rebuilds every object once. Then the .d files hold it.
    r = make(f"BUILD={spell[second]}", f"{spell[second]}/{OBJ}")
    assert r.returncode == 0, r.stderr
    with open(obj[:-2] + ".d") as f:
        assert f.readline().startswith(f"{spell[second]}/{OBJ}:")
    # The object is newer than its source and older than a header it
    # includes, as after an edit of that header.
    touch_header(obj)
    assert make_q(spell[second], f"{spell[second]}/{OBJ}") == 1, \
        "the header change did not reach the object"


def make_q(build, target=None):
    """0 when the target is up to date, 1 when make would rebuild it."""
    return make("-q", f"BUILD={build}", target or f"{build}/{OBJ}").returncode


def touch_header(obj):
    t = max(os.stat(SRC).st_mtime, time.time() - 10)
    os.utime(obj, (t, t))
    now = max(time.time(), t + 1)
    os.utime(HDR, (now, now))


def test_same_spelling_rebuilds_nothing(deptest):
    for build in (REL, deptest):
        r = make(f"BUILD={build}")
        assert r.returncode == 0, r.stderr
        assert make_q(build, "all") == 0, build


def test_spelling_switch_twice(deptest):
    obj = os.path.join(deptest, OBJ)
    for build in (REL, deptest, REL):
        assert make_q(build) == 1, f"{build}: the new spelling did not rebuild"
        r = make(f"BUILD={build}", f"{build}/{OBJ}")
        assert r.returncode == 0, r.stderr
        assert make_q(build) == 0
    touch_header(obj)
    assert make_q(REL) == 1, "the header change did not reach the object"


def test_build_outside_the_project(deptest, tmp_path):
    out = str(tmp_path / "out")
    obj = os.path.join(out, OBJ)
    r = make(f"BUILD={out}")
    assert r.returncode == 0, r.stderr
    with open(obj[:-2] + ".d") as f:
        assert f.readline().startswith(f"{out}/{OBJ}:")
    assert make_q(out) == 0
    touch_header(obj)
    assert make_q(out) == 1, "the header change did not reach the object"


MTOOLS = ("mformat", "mcopy", "mmd", "mdir", "mtype")
need_mtools = pytest.mark.skipif(not all(shutil.which(t) for t in MTOOLS),
                                 reason="mtools not installed")


@need_mtools
@pytest.mark.parametrize("first, second", [("rel", "abs"), ("abs", "rel")])
def test_sample_sdimage_with_either_spelling(deptest, tmp_path, first, second):
    """The sample project names $(BUILD) in SDIMAGE_FILES before the include."""
    project = tmp_path / "sample-project"
    shutil.copytree(os.path.join(ROOT, "tests", "fixtures", "sample-project"), project,
                    ignore=shutil.ignore_patterns("build"))
    spell = {"rel": "build", "abs": str(project / "build")}
    env = {"PATH": os.environ.get("PATH", ""), "HOME": os.environ.get("HOME", "")}
    for build in (spell[first], spell[second]):
        r = subprocess.run(["make", "-s", "-C", str(project), f"ROBCO_CORE={ROOT}", "CI=1",
                            f"BUILD={build}", "sdimage"], env=env, stdout=subprocess.DEVNULL,
                           stderr=subprocess.PIPE, text=True, timeout=120)
        assert r.returncode == 0, f"BUILD={build}: {r.stderr}"
        assert (project / "build" / "sd.img").is_file()


@pytest.mark.parametrize("var", ["HOST_BUILD=build/host", "HOST_BUILD={root}/build/host"])
def test_host_build_keeps_its_spelling(var):
    value = var.format(root=ROOT).split("=", 1)[1]
    out = subprocess.run(["make", "-pn", "-C", ROOT, var.format(root=ROOT), "venv"],
                         capture_output=True, text=True, timeout=60).stdout
    line = next(x for x in out.splitlines() if x.startswith("HOST_GW_OUT "))
    assert line.split("=", 1)[1].strip() == f"{value}/gccwarn", line
