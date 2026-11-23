"""tools/ci_local.sh and the ci-local make targets, with a fake docker.

The fake docker logs its arguments and answers from files in its own
directory, so these tests need no Docker.
"""

import pathlib
import shutil
import subprocess

import pytest

from harness.env import minimal_env, prepend_path

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools" / "ci_local.sh"
SAMPLE = ROOT / "tests" / "fixtures" / "sample-project"
TAG = "abc123def456"
LOCAL = f"robco-ci:{TAG}"
PUBLISHED = f"ghcr.io/robco-industries/robco-ci:{TAG}"
CANARY = "ci_local_canary_value_7f3e"

FAKE_DOCKER = """#!/bin/sh
d=$(dirname "$0")
{ echo "--- $1"; for a in "$@"; do echo "$a"; done; } >> "$d/log"
case "$1" in
  info) [ -f "$d/down" ] && exit 1; exit 0 ;;
  image) grep -qx "$3" "$d/images" 2>/dev/null ;;
  pull) grep -qx "$3" "$d/remote" 2>/dev/null ;;
  *) exit 0 ;;
esac
"""

FAKE_GIT = """#!/bin/sh
echo 1111111111111111111111111111111111111111
"""


@pytest.fixture
def stub(tmp_path):
    d = tmp_path / "stub"
    d.mkdir()
    for name, text in (("docker", FAKE_DOCKER), ("git", FAKE_GIT)):
        (d / name).write_text(text)
        (d / name).chmod(0o755)
    return d


@pytest.fixture
def proj(tmp_path):
    """A project with the library in external/robco-core and junk to skip."""
    p = tmp_path / "proj"
    core = p / "external" / "robco-core"
    (core / "tools").mkdir(parents=True)
    (core / "ci").mkdir()
    shutil.copy(SCRIPT, core / "tools" / "ci_local.sh")
    (core / "ci" / "IMAGE_TAG").write_text(TAG + "\n")
    (p / "src").mkdir()
    (p / "src" / "main.c").write_text("int main(void) { return 0; }\n")
    for junk in ("build/old.o", ".venv/bin/python", ".git/config", "external/robco-core/.git"):
        (p / junk).parent.mkdir(parents=True, exist_ok=True)
        (p / junk).write_text("junk\n")
    return p


def images(stub, *names, remote=()):
    (stub / "images").write_text("".join(n + "\n" for n in names))
    (stub / "remote").write_text("".join(n + "\n" for n in remote))


def run(proj, stub, *args, **extra):
    extra.setdefault("PATH", prepend_path(stub))
    env = minimal_env(CI_LOCAL_SECRET=CANARY, **extra)
    for k in ("CI_IMAGE", "CI_LOCAL_CPUS", "CI_LOCAL_TARGETS"):
        if k not in extra:
            env.pop(k, None)
    return subprocess.run(["sh", "external/robco-core/tools/ci_local.sh", *args],
                          cwd=proj, env=env, capture_output=True, text=True)


def docker_run(stub):
    """The argument list of the last docker run call."""
    calls = (stub / "log").read_text().split("--- ")
    runs = [c.splitlines()[1:] for c in calls if c.startswith("run\n")]
    assert runs, "docker run was not called"
    return runs[-1]


def test_default_run(proj, stub):
    images(stub, LOCAL)
    r = run(proj, stub)
    assert r.returncode == 0, r.stderr
    argv = docker_run(stub)
    assert argv[argv.index("--cpus") + 1] == "4"
    assert argv[-5:] == [LOCAL, "make", "CI=1", "all", "test-host"]
    assert f"ci-local: {LOCAL}, 4 CPUs: make CI=1 all test-host" in r.stdout


def test_arguments_and_variables(proj, stub):
    images(stub, LOCAL)
    r = run(proj, stub, "-k", "test-target", "BOARD=raspi0", CI_LOCAL_CPUS="2")
    assert r.returncode == 0, r.stderr
    argv = docker_run(stub)
    assert argv[argv.index("--cpus") + 1] == "2"
    assert argv[-4:] == ["CI=1", "-k", "test-target", "BOARD=raspi0"]
    run(proj, stub, CI_LOCAL_TARGETS="lint test-host")
    assert docker_run(stub)[-3:] == ["CI=1", "lint", "test-host"]


@pytest.mark.parametrize("args,extra,msg", [
    (["-j"], {}, "give -j a number"),
    ([], {"CI_LOCAL_CPUS": "all"}, "CI_LOCAL_CPUS must be a number"),
    (["--root"], {}, "--root needs a directory"),
])
def test_bad_arguments(proj, stub, args, extra, msg):
    images(stub, LOCAL)
    r = run(proj, stub, *args, **extra)
    assert r.returncode == 1
    assert msg in r.stderr


def test_numbered_jobs_pass(proj, stub):
    images(stub, LOCAL)
    assert run(proj, stub, "-j4", "all").returncode == 0
    assert docker_run(stub)[-2:] == ["-j4", "all"]


def test_image_order(proj, stub):
    images(stub, LOCAL, PUBLISHED)
    run(proj, stub)
    assert LOCAL in docker_run(stub)
    images(stub, PUBLISHED)
    run(proj, stub)
    assert PUBLISHED in docker_run(stub)
    images(stub, remote=[PUBLISHED])
    assert run(proj, stub).returncode == 0
    assert PUBLISHED in docker_run(stub)
    images(stub, LOCAL, "mine:1")
    run(proj, stub, CI_IMAGE="mine:1")
    assert "mine:1" in docker_run(stub)
    assert LOCAL not in docker_run(stub)


def test_no_image(proj, stub):
    images(stub)
    r = run(proj, stub)
    assert r.returncode == 1
    assert "Build it with: make ci-local-image" in r.stderr
    r = run(proj, stub, CI_IMAGE="mine:1")
    assert r.returncode == 1
    assert "image mine:1 not found" in r.stderr


def test_docker_missing(proj, stub, tmp_path):
    (stub / "docker").unlink()
    r = run(proj, stub, PATH=f"{stub}:/usr/bin:/bin")
    assert r.returncode == 1
    assert r.stderr.strip() == "ci-local: docker not found. Install Docker to run ci-local."


def test_docker_not_running(proj, stub):
    (stub / "down").write_text("")
    r = run(proj, stub)
    assert r.returncode == 1
    assert r.stderr.strip() == "ci-local: Docker is not running. Start it and try again."


def test_no_host_environment_reaches_docker(proj, stub):
    images(stub, LOCAL)
    assert run(proj, stub, HOST_CC="gcc").returncode == 0
    argv = docker_run(stub)
    assert not any(CANARY in a for a in argv)
    assert not any(a.startswith("--env") for a in argv)
    envs = [argv[i + 1] for i, a in enumerate(argv) if a == "-e"]
    assert envs == ["HOME=/tmp"]
    assert "HOST_CC=gcc" not in argv


def test_tree_copy(proj, stub):
    images(stub, LOCAL)
    (proj / "build" / "ci-local" / "src").mkdir(parents=True)
    (proj / "build" / "ci-local" / "src" / "stale").write_text("x")
    assert run(proj, stub).returncode == 0
    src = proj / "build" / "ci-local" / "src"
    assert (src / "src" / "main.c").exists()
    assert not (src / "build").exists()
    assert not (src / ".venv").exists()
    assert not (src / "stale").exists()
    head = "1" * 40 + "\n"
    assert sorted(p.name for p in (src / ".git").iterdir()) == ["HEAD"]
    assert (src / ".git" / "HEAD").read_text() == head
    assert (src / "external" / "robco-core" / ".git" / "HEAD").read_text() == head
    argv = docker_run(stub)
    assert f"{src}:/work" in argv


def test_build_image(proj, stub):
    r = run(proj, stub, "--build-image")
    assert r.returncode == 0, r.stderr
    calls = (stub / "log").read_text()
    core = proj / "external" / "robco-core"
    assert f"--- build\nbuild\n-f\n{core}/ci/Dockerfile\n-t\n{LOCAL}\n{core}/ci\n" in calls


def test_make_targets_in_a_project(tmp_path, stub):
    dest = tmp_path / "sample-project"
    shutil.copytree(SAMPLE, dest, ignore=shutil.ignore_patterns("build"))
    images(stub, f"robco-ci:{(ROOT / 'ci' / 'IMAGE_TAG').read_text().strip()}")
    env = minimal_env(PATH=prepend_path(stub))

    def make(*args):
        return subprocess.run(["make", "--no-print-directory", "-C", str(dest),
                               f"ROBCO_CORE={ROOT}", *args],
                              env=env, capture_output=True, text=True)

    r = make("ci-local")
    assert r.returncode == 0, r.stderr
    assert docker_run(stub)[-3:] == ["CI=1", "all", "test-host"]
    r = make("ci-local", "CI_LOCAL_TARGETS=test-host BOARD=raspi0", "CI_LOCAL_CPUS=3")
    assert r.returncode == 0, r.stderr
    argv = docker_run(stub)
    assert argv[argv.index("--cpus") + 1] == "3"
    assert argv[-3:] == ["CI=1", "test-host", "BOARD=raspi0"]
    assert (dest / "build" / "ci-local" / "src" / "Makefile").exists()
    r = make("ci-local-image")
    assert r.returncode == 0, r.stderr
    assert f"{ROOT}/ci/Dockerfile" in (stub / "log").read_text()
