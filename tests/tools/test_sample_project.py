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
