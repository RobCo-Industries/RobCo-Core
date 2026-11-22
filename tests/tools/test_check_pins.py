"""tools/check_pins.sh on fixture workflows and hand-made submodule git files.

The script must work without git: CI containers refuse git on a checkout
owned by another user. Most cases run with a git stub that exits 128.
"""

import os
import pathlib
import shutil
import subprocess

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools" / "check_pins.sh"
FIXTURES = ROOT / "tests" / "fixtures" / "check-pins"
COMMIT = "1" * 40
TAG = "abc123def456"


def make_project(tmp_path, head=COMMIT, refs=None, packed=None, gitdir_file=True):
    """A project with external/robco-core and its git files, no git needed."""
    proj = tmp_path / "proj"
    core = proj / "external" / "robco-core"
    (core / "tools").mkdir(parents=True)
    (core / "ci").mkdir()
    shutil.copy(SCRIPT, core / "tools" / "check_pins.sh")
    (core / "ci" / "IMAGE_TAG").write_text(TAG + "\n")
    if gitdir_file:
        gitdir = proj / ".git" / "modules" / "robco-core"
        gitdir.mkdir(parents=True)
        (core / ".git").write_text("gitdir: ../../.git/modules/robco-core\n")
    else:
        gitdir = core / ".git"
        gitdir.mkdir()
    (gitdir / "HEAD").write_text(head + "\n")
    for name, value in (refs or {}).items():
        p = gitdir / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(value + "\n")
    if packed:
        (gitdir / "packed-refs").write_text("# pack-refs with: peeled fully-peeled sorted\n"
                                            + "".join(f"{v} {k}\n" for k, v in packed.items()))
    (proj / ".github" / "workflows").mkdir(parents=True)
    return proj


@pytest.fixture
def no_git(tmp_path):
    """A PATH whose git exits 128 like git on a checkout it does not trust."""
    stub = tmp_path / "stub"
    stub.mkdir()
    git = stub / "git"
    git.write_text("#!/bin/sh\necho 'fatal: detected dubious ownership' >&2\nexit 128\n")
    git.chmod(0o755)
    return dict(os.environ, PATH=f"{stub}{os.pathsep}{os.environ['PATH']}")


def run(proj, fixture, env=None):
    wf = proj / ".github" / "workflows" / "ci.yml"
    shutil.copy(FIXTURES / fixture, wf)
    return subprocess.run(["bash", "external/robco-core/tools/check_pins.sh",
                           ".github/workflows/ci.yml"], cwd=proj, env=env,
                          capture_output=True, text=True)


def test_good_detached(tmp_path, no_git):
    r = run(make_project(tmp_path), "good.yml", no_git)
    assert r.returncode == 0, r.stderr
    assert r.stdout.strip() == f"check-pins: ok ({COMMIT}, {TAG})"


def test_good_through_a_ref_and_packed_refs(tmp_path, no_git):
    proj = make_project(tmp_path / "a", head="ref: refs/heads/main",
                        refs={"refs/heads/main": COMMIT})
    assert run(proj, "good.yml", no_git).returncode == 0
    proj = make_project(tmp_path / "b", head="ref: refs/heads/main",
                        packed={"refs/heads/main": COMMIT})
    assert run(proj, "good.yml", no_git).returncode == 0


def test_good_with_a_git_directory(tmp_path, no_git):
    assert run(make_project(tmp_path, gitdir_file=False), "good.yml", no_git).returncode == 0


def test_stale_commit(tmp_path, no_git):
    r = run(make_project(tmp_path), "stale-commit.yml", no_git)
    assert r.returncode == 1
    assert f"uses robco-ci.yml@{'2' * 40}, the submodule is at {COMMIT}" in r.stderr


def test_stale_tag(tmp_path, no_git):
    r = run(make_project(tmp_path), "stale-tag.yml", no_git)
    assert r.returncode == 1
    assert f"image-tag 000000000000, external/robco-core/ci/IMAGE_TAG is {TAG}" in r.stderr


def test_no_call(tmp_path, no_git):
    r = run(make_project(tmp_path), "no-call.yml", no_git)
    assert r.returncode == 1
    assert "has no job that calls RobCo-Core/.github/workflows/robco-ci.yml" in r.stderr


@pytest.mark.parametrize("fixture", ["quoted.yml", "crlf.yml"])
def test_good_quoted_and_crlf(tmp_path, no_git, fixture):
    r = run(make_project(tmp_path), fixture, no_git)
    assert r.returncode == 0, r.stderr


def test_each_calling_job_is_checked(tmp_path, no_git):
    r = run(make_project(tmp_path), "two-jobs-one-wrong.yml", no_git)
    assert r.returncode == 1
    assert f"job ci-extra uses image-tag 000000000000, external/robco-core/ci/IMAGE_TAG is {TAG}" in r.stderr
    assert "job ci " not in r.stderr


@pytest.mark.parametrize("fixture", ["tag-on-other-job.yml", "commented-tag.yml"])
def test_tag_must_be_in_the_calling_job(tmp_path, no_git, fixture):
    """An image-tag in another job, or in a comment, does not count."""
    r = run(make_project(tmp_path), fixture, no_git)
    assert r.returncode == 1
    assert "job ci sets no image-tag" in r.stderr


def test_no_python3(tmp_path, no_git):
    stub = tmp_path / "nopy"
    stub.mkdir()
    for tool in ("bash", "tr", "head", "awk", "dirname", "git"):
        src = shutil.which(tool, path=no_git["PATH"])
        (stub / tool).symlink_to(src)
    r = run(make_project(tmp_path), "good.yml", dict(no_git, PATH=str(stub)))
    assert r.returncode == 1 and "python3 is required" in r.stderr


def test_unresolved_ref_and_missing_files(tmp_path, no_git):
    r = run(make_project(tmp_path / "a", head="ref: refs/heads/gone"), "good.yml", no_git)
    assert r.returncode == 1 and "cannot resolve refs/heads/gone" in r.stderr
    proj = make_project(tmp_path / "b")
    (proj / "external" / "robco-core" / "ci" / "IMAGE_TAG").unlink()
    r = run(proj, "good.yml", no_git)
    assert r.returncode == 1 and "IMAGE_TAG is missing" in r.stderr
    r = subprocess.run(["bash", str(SCRIPT)], capture_output=True, text=True)
    assert r.returncode == 2 and "usage" in r.stderr


@pytest.mark.skipif(shutil.which("git") is None, reason="git not installed")
def test_git_cross_check_warns(tmp_path):
    """When git works, a checkout that differs from the recorded commit warns."""
    proj = make_project(tmp_path)
    env = dict(os.environ, GIT_CONFIG_NOSYSTEM="1", HOME=str(tmp_path))
    git = ["git", "-C", str(proj)]
    subprocess.run(git + ["init", "-q"], check=True, env=env)
    r = subprocess.run(git + ["update-index", "--add", "--cacheinfo",
                              f"160000,{'3' * 40},external/robco-core"],
                       env=env, capture_output=True, text=True)
    if r.returncode != 0:
        pytest.skip(f"git cannot record a gitlink here: {r.stderr.strip()}")
    r = run(proj, "good.yml", env)
    assert r.returncode == 0, r.stderr
    assert f"the project records {'3' * 40}" in r.stderr


@pytest.mark.parametrize("fixture", ["flow.yml", "flow-quoted.yml"])
def test_good_single_line_flow_mapping(tmp_path, no_git, fixture):
    r = run(make_project(tmp_path), fixture, no_git)
    assert r.returncode == 0, r.stderr
    assert r.stdout.strip() == f"check-pins: ok ({COMMIT}, {TAG})"


def test_stale_tag_in_flow_mapping(tmp_path, no_git):
    r = run(make_project(tmp_path), "flow-stale-tag.yml", no_git)
    assert r.returncode == 1
    assert f"job ci uses image-tag 000000000000, external/robco-core/ci/IMAGE_TAG is {TAG}" in r.stderr


@pytest.mark.parametrize("fixture, line, why", [
    ("flow-multiline.yml", 8, "a flow mapping that spans several lines"),
    ("anchor.yml", 6, "an anchor or alias"),
])
def test_unsupported_yaml_is_named(tmp_path, no_git, fixture, line, why):
    """YAML the scan cannot read fails with its line, never ok or missing."""
    r = run(make_project(tmp_path), fixture, no_git)
    assert r.returncode == 1
    assert (f"check-pins: .github/workflows/ci.yml:{line}: cannot read {why}; "
            "this script supports block mappings and single-line flow mappings only") in r.stderr
    assert "sets no image-tag" not in r.stderr and "ok" not in r.stdout
    assert "has no job" not in r.stderr
