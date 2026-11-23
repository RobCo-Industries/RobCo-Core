"""tools/check_doc_links.sh on temp project trees with hand-made submodule git files.

The script must work without git: CI containers refuse git on a checkout
owned by another user. Every case runs with a git stub that exits 128.
"""

import os
import pathlib
import subprocess

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools" / "check_doc_links.sh"
COMMIT = "1" * 40
OLD = "2" * 40
SUB = "external/robco-core"


def link(commit, kind="blob", path="docs/guide/API.md"):
    return f"https://github.com/RobCo-Industries/RobCo-Core/{kind}/{commit}/{path}"


def make_project(tmp_path, head=COMMIT, refs=None, packed=None, gitdir_file=True):
    """A project with external/robco-core and its git files, no git needed."""
    proj = tmp_path / "proj"
    core = proj / SUB
    core.mkdir(parents=True)
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
        (gitdir / "packed-refs").write_text("".join(f"{v} {k}\n" for k, v in packed.items()))
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


def run(proj, env, *args, sub=SUB):
    return subprocess.run(["bash", str(SCRIPT), str(proj), sub, *args],
                          capture_output=True, text=True, env=env)


def write(proj, rel, text):
    p = proj / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text)


def test_current_links_pass(tmp_path, no_git):
    proj = make_project(tmp_path)
    write(proj, "README.md", f"See [API]({link(COMMIT)}).\n")
    write(proj, "docs/a.md", f"x\n[t]({link(COMMIT, 'tree', 'src')}) [c]({link(COMMIT, 'commit', '')})\n")
    r = run(proj, no_git)
    assert r.returncode == 0, r.stderr
    assert f"ok ({COMMIT})" in r.stdout


def test_stale_link_fails_with_file_and_line(tmp_path, no_git):
    proj = make_project(tmp_path)
    write(proj, "docs/guide.md", f"one\ntwo\n[old]({link(OLD)})\n")
    r = run(proj, no_git)
    assert r.returncode == 1
    assert f"docs/guide.md:3 links RobCo-Core at {OLD}, the submodule is at {COMMIT}" in r.stderr


def test_two_links_on_one_line(tmp_path, no_git):
    proj = make_project(tmp_path)
    write(proj, "a.md", f"[a]({link(COMMIT)}) [b]({link(OLD)})\n")
    r = run(proj, no_git)
    assert r.returncode == 1
    assert f"a.md:1 links RobCo-Core at {OLD}" in r.stderr
    assert r.stderr.count("links RobCo-Core") == 1


def test_changelog_and_agent_files_are_ignored(tmp_path, no_git):
    proj = make_project(tmp_path)
    for name in ("CHANGELOG.md", "CLAUDE.md", "AGENTS.md", "sub/CHANGELOG.md"):
        write(proj, name, f"[old]({link(OLD)})\n")
    r = run(proj, no_git)
    assert r.returncode == 0, r.stderr


def test_default_and_extra_excludes(tmp_path, no_git):
    proj = make_project(tmp_path)
    for d in ("build", "sdk/examples/x/build", ".venv", "vendor", "third_party",
              "node_modules", SUB + "/docs"):
        write(proj, f"{d}/x.md", f"[old]({link(OLD)})\n")
    write(proj, "gen/out/x.md", f"[old]({link(OLD)})\n")
    r = run(proj, no_git)
    assert r.returncode == 1
    assert "gen/out/x.md:1" in r.stderr
    assert r.stderr.count("links RobCo-Core") == 1
    r = run(proj, no_git, "gen/out")
    assert r.returncode == 0, r.stderr


def test_branch_head_through_refs_and_packed_refs(tmp_path, no_git):
    proj = make_project(tmp_path, head="ref: refs/heads/main",
                        refs={"refs/heads/main": COMMIT})
    write(proj, "a.md", f"[a]({link(COMMIT)})\n")
    assert run(proj, no_git).returncode == 0
    proj2 = make_project(tmp_path / "p2", head="ref: refs/heads/main",
                         packed={"refs/heads/main": COMMIT}, gitdir_file=False)
    write(proj2, "a.md", f"[a]({link(OLD)})\n")
    r = run(proj2, no_git)
    assert r.returncode == 1
    assert "a.md:1" in r.stderr


def test_no_submodule_is_a_clear_error(tmp_path, no_git):
    proj = tmp_path / "proj"
    proj.mkdir()
    write(proj, "a.md", "text\n")
    r = run(proj, no_git)
    assert r.returncode == 1
    assert f"{SUB} is missing" in r.stderr
    (proj / SUB).mkdir(parents=True)
    r = run(proj, no_git)
    assert r.returncode == 1
    assert "no HEAD" in r.stderr and "submodule update --init" in r.stderr


def test_bad_usage(tmp_path, no_git):
    r = subprocess.run(["bash", str(SCRIPT), str(tmp_path)], capture_output=True,
                       text=True, env=no_git)
    assert r.returncode == 2
    assert "usage" in r.stderr


@pytest.mark.parametrize("sub", ["./" + SUB, SUB + "/", ".//" + SUB, "external//robco-core/"])
def test_submodule_path_forms_skip_its_markdown(tmp_path, no_git, sub):
    proj = make_project(tmp_path)
    write(proj, SUB + "/docs/x.md", f"[old]({link(OLD)})\n")
    write(proj, "docs/out.md", f"[old]({link(OLD)})\n")
    r = run(proj, no_git, sub=sub)
    assert r.returncode == 1
    assert "docs/out.md:1" in r.stderr
    assert r.stderr.count("links RobCo-Core") == 1, r.stderr


@pytest.mark.parametrize("ex", ["./docs/gen/", "docs//gen", "docs/gen/"])
def test_exclude_path_forms(tmp_path, no_git, ex):
    proj = make_project(tmp_path)
    write(proj, "docs/gen/x.md", f"[old]({link(OLD)})\n")
    write(proj, "docs/out.md", f"[old]({link(OLD)})\n")
    r = run(proj, no_git, ex)
    assert r.returncode == 1
    assert "docs/out.md:1" in r.stderr
    assert r.stderr.count("links RobCo-Core") == 1, r.stderr


@pytest.mark.parametrize("root_form", ["dot", "absolute"])
def test_root_forms(tmp_path, no_git, root_form):
    proj = make_project(tmp_path)
    write(proj, SUB + "/docs/x.md", f"[old]({link(OLD)})\n")
    write(proj, "docs/gen/x.md", f"[old]({link(OLD)})\n")
    write(proj, "docs/out.md", f"[old]({link(OLD)})\n")
    root = "." if root_form == "dot" else str(proj.resolve())
    r = subprocess.run(["bash", str(SCRIPT), root, "./" + SUB, "./docs/gen/"],
                       capture_output=True, text=True, env=no_git, cwd=proj)
    assert r.returncode == 1
    assert "docs/out.md:1" in r.stderr
    assert r.stderr.count("links RobCo-Core") == 1, r.stderr


@pytest.mark.parametrize("args", [
    ["/abs/robco-core"], [".", ], ["./"], ["../robco-core"], ["external/../x"],
    [SUB, "/abs/docs"], [SUB, "docs/../.."],
])
def test_unsupported_paths_are_usage_errors(tmp_path, no_git, args):
    proj = make_project(tmp_path)
    r = run(proj, no_git, *args[1:], sub=args[0])
    assert r.returncode == 2, r.stderr
    assert "usage" in r.stderr
