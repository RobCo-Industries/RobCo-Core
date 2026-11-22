"""Build tests/fixtures/sample-project from a copy outside the repository.

The copy lives in a temporary directory and reaches RobCo-Core through
ROBCO_CORE only, so every path in mk/robco.mk and mk/host.mk that assumes
the RobCo-Core root fails here.
"""

import os
import shutil
import subprocess
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SAMPLE = os.path.join(ROOT, "tests", "fixtures", "sample-project")
sys.path.insert(0, os.path.join(ROOT, "tools"))
import rimg  # noqa: E402

need_cross = pytest.mark.skipif(shutil.which("arm-none-eabi-gcc") is None,
                                reason="arm-none-eabi-gcc not installed")


def make(project, *args, check=True):
    r = subprocess.run(["make", "--no-print-directory", "-C", str(project),
                        f"ROBCO_CORE={ROOT}", *args], capture_output=True, text=True)
    if check and r.returncode != 0:
        raise AssertionError(f"make {args} failed:\n{r.stdout}\n{r.stderr}")
    return r


@pytest.fixture
def project(tmp_path_factory):
    base = tmp_path_factory.mktemp("sibling")
    dest = base / "sample-project"
    shutil.copytree(SAMPLE, dest, ignore=shutil.ignore_patterns("build"))
    assert not str(dest.resolve()).startswith(ROOT + os.sep)
    return dest


@need_cross
def test_image_builds_outside_the_tree(project):
    r = make(project, "CI=1")
    assert "warning" not in (r.stdout + r.stderr).lower()
    with open(project / "build" / "sample.img", "rb") as f:
        h = rimg.validate(f.read())
    assert h["name"].rstrip(b"\0") == b"sample"


def test_host_tests_run_outside_the_tree(project):
    r = make(project, "test-host")
    assert "test-host: all passed" in r.stdout
    assert "test_greet: 2 tests, 2 passed" in r.stdout
    assert (project / "build" / "host" / "test_greet").exists()
    # The core's own tests and tools stay out of a project build.
    assert not (project / "build" / "host" / "fs_tool").exists()


def test_host_tests_with_the_file_system(project):
    r = make(project, "test-host", "HOST_CORE_SETS=fs fatfs", "HOST_BUILD=build/hostfs")
    assert "test-host: all passed" in r.stdout
    assert (project / "build" / "hostfs" / "fs" / "fs.o").exists()


def test_host_build_dir_and_failure(project):
    with open(project / "tests" / "host" / "test_fail.c", "w") as f:
        f.write('#include "rc_test.h"\nTEST(fails) { CHECK_EQ(1, 2); }\n')
    r = make(project, "test-host", "HOST_BUILD=out/h", check=False)
    assert r.returncode != 0
    assert "test-host: FAILED: test_fail" in r.stderr
    assert (project / "out" / "h" / "test_greet").exists()


def test_fs_without_fatfs_is_refused(project):
    r = make(project, "test-host", "HOST_CORE_SETS=fs", check=False)
    assert r.returncode != 0
    assert "fs needs fatfs" in r.stderr


MTOOLS = ("mformat", "mcopy", "mmd", "mdir", "mtype")
need_mtools = pytest.mark.skipif(not all(shutil.which(t) for t in MTOOLS),
                                 reason="mtools not installed")
PART_OFF = 2048 * 512


def mt(*args):
    env = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    return subprocess.run(list(args), check=True, env=env, capture_output=True).stdout


@need_cross
@need_mtools
def test_sdimage_files_and_hook(project):
    r = make(project, "CI=1", "sdimage")
    out = r.stdout + r.stderr
    assert "warning" not in out.lower(), out
    card = f"{project / 'build' / 'sd.img'}@@{PART_OFF}"
    listing = sorted(mt("mdir", "-/", "-b", "-i", card, "::").decode().split())
    assert listing == ["::/DATA/", "::/DATA/HELLO.TXT", "::/DATA/STAMP.TXT",
                               "::/SAMPLE.IMG"]
    with open(project / "build" / "sample.img", "rb") as f:
        assert mt("mtype", "-i", card, "::/SAMPLE.IMG") == f.read()
    with open(project / "data" / "hello.txt", "rb") as f:
        assert mt("mtype", "-i", card, "::/DATA/HELLO.TXT") == f.read()
    assert mt("mtype", "-i", card, "::/DATA/STAMP.TXT") == b"staged by sample\n"
    # A second run starts from an empty stage and gives the same card.
    make(project, "CI=1", "sdimage")
    assert sorted(mt("mdir", "-/", "-b", "-i", card, "::").decode().split()) == listing


def test_sdimage_rejects_a_bad_pair(project):
    r = make(project, "sdimage", "SDIMAGE_FILES=data/hello.txt", check=False)
    assert r.returncode != 0
    assert "is not source:destination" in r.stderr


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def staged(project):
    """The staged tree, as {relative path: content}."""
    stage = project / "build" / "sdstage"
    return {p.relative_to(stage).as_posix(): p.read_text()
            for p in sorted(stage.rglob("*")) if p.is_file()}


def test_sdimage_pair_semantics(project):
    """Files go to exact paths, directories merge, later pairs win."""
    write(project / "card" / "APP" / "OLD.TXT", "card old\n")
    write(project / "card" / "APP" / "KEEP.TXT", "card keep\n")
    write(project / "assets" / "OLD.TXT", "assets old\n")
    write(project / "assets" / "SUB" / "A.TXT", "assets a\n")
    write(project / "more" / "B.TXT", "more b\n")
    write(project / "cfg" / "app.cfg", "cfg\n")
    write(project / "cfg" / "over.txt", "over\n")
    pairs = " ".join([
        "cfg/app.cfg:CONF/APP.CFG",     # file to a new path
        "cfg/app.cfg:APP/",             # file into a directory
        "more:NEW/DIR",                 # directory to a new destination
        "assets:APP",                   # directory into SDIMAGE_DIR's APP
        "cfg/over.txt:APP/SUB/A.TXT",   # overwrites the file from assets
    ])
    r = make(project, "sdimage", "SDIMAGE_DIR=card", f"SDIMAGE_FILES={pairs}", check=False)
    assert staged(project) == {
        "APP/KEEP.TXT": "card keep\n",
        "APP/OLD.TXT": "assets old\n",
        "APP/SUB/A.TXT": "over\n",
        "APP/app.cfg": "cfg\n",
        "CONF/APP.CFG": "cfg\n",
        "DATA/STAMP.TXT": "staged by sample\n",
        "NEW/DIR/B.TXT": "more b\n",
    }, r.stdout + r.stderr
    if not all(shutil.which(t) for t in MTOOLS):
        return
    assert r.returncode == 0, r.stdout + r.stderr
    card = f"{project / 'build' / 'sd.img'}@@{PART_OFF}"
    assert mt("mtype", "-i", card, "::/APP/SUB/A.TXT") == b"over\n"
    assert mt("mtype", "-i", card, "::/APP/OLD.TXT") == b"assets old\n"
    assert mt("mtype", "-i", card, "::/NEW/DIR/B.TXT") == b"more b\n"


def test_sdimage_rejects_a_directory_on_a_file(project):
    write(project / "cfg" / "app.cfg", "cfg\n")
    write(project / "more" / "B.TXT", "b\n")
    r = make(project, "sdimage", "SDIMAGE_FILES=cfg/app.cfg:X more:X", check=False)
    assert r.returncode != 0 and "is a directory, X a file" in r.stderr
    r = make(project, "sdimage", "SDIMAGE_FILES=more:X cfg/app.cfg:X", check=False)
    assert r.returncode != 0 and "end it with / to copy into it" in r.stderr
    r = make(project, "sdimage", "SDIMAGE_FILES=nothing:X", check=False)
    assert r.returncode != 0
