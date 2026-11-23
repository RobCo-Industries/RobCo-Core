"""tools/check_doc_links.sh on temp project trees with hand-made submodule git files.

The script must work without git: CI containers refuse git on a checkout
owned by another user. Every case runs with a git stub that exits 128.
"""

import os
import pathlib
import subprocess

import pytest

from harness.env import minimal_env, prepend_path

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
    return minimal_env(PATH=prepend_path(stub))


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


@pytest.mark.parametrize("url", [
    link("develop", "blob", "README.md"),
    link("feature-x", "tree", ""),
    link("dead-code", "blob", "x.md"),
    link("beef", "tree", "src"),
    link("abc123", "blob", "x.md"),
    link("v1.0", "tree", ""),
])
def test_branch_and_tag_links_are_not_pins(tmp_path, no_git, url):
    proj = make_project(tmp_path)
    write(proj, "a.md", f"[b]({url})\n")
    r = run(proj, no_git)
    assert r.returncode == 0, r.stderr


@pytest.mark.parametrize("short", [COMMIT[:7], COMMIT[:12], COMMIT[:39]])
def test_abbreviated_commit_is_an_error(tmp_path, no_git, short):
    proj = make_project(tmp_path)
    write(proj, "a.md", f"[s]({link(short)})\n")
    r = run(proj, no_git)
    assert r.returncode == 1
    assert f"a.md:1 links RobCo-Core at abbreviated commit {short}, use the full hash {COMMIT}" in r.stderr


def test_uppercase_hash_compares_case_insensitively(tmp_path, no_git):
    proj = make_project(tmp_path, head="ab" * 20)
    write(proj, "a.md", f"[u]({link('AB' * 20)})\n")
    assert run(proj, no_git).returncode == 0
    write(proj, "a.md", f"[u]({link('CD' * 20)})\n")
    r = run(proj, no_git)
    assert r.returncode == 1
    assert f"at {'CD' * 20}, the submodule is at {'ab' * 20}" in r.stderr


def test_several_links_on_one_line_mixed(tmp_path, no_git):
    proj = make_project(tmp_path)
    write(proj, "a.md", f"[a]({link(COMMIT)}) [b]({link('develop')}) [c]({link(OLD)}) [d]({link('1234567')})\n")
    r = run(proj, no_git)
    assert r.returncode == 1
    assert f"a.md:1 links RobCo-Core at {OLD}," in r.stderr
    assert "abbreviated commit 1234567" in r.stderr
    assert r.stderr.count("links RobCo-Core") == 2, r.stderr


def test_reference_definitions(tmp_path, no_git):
    proj = make_project(tmp_path)
    write(proj, "a.md", f"[x][r] [y][s]\n\n[r]: {link(OLD)}\n[s]: <{link('main')}>\n[t]: {link(COMMIT)} \"title\"\n")
    r = run(proj, no_git)
    assert r.returncode == 1
    assert f"a.md:3 links RobCo-Core at {OLD}," in r.stderr
    assert r.stderr.count("links RobCo-Core") == 1, r.stderr


def test_hash_ends_at_delimiters(tmp_path, no_git):
    proj = make_project(tmp_path)
    base = "https://github.com/RobCo-Industries/RobCo-Core/blob/"
    write(proj, "a.md", f"{base}{COMMIT}#L1 {base}{COMMIT}?plain=1 '{base}{COMMIT}' <{base}{COMMIT}>\n{base}{COMMIT}\n")
    r = run(proj, no_git)
    assert r.returncode == 0, r.stderr
    write(proj, "a.md", f"{base}{COMMIT}0/x.md\n")
    assert run(proj, no_git).returncode == 0  # 41 hex is a name, not a pin


def test_code_blocks_are_checked_too(tmp_path, no_git):
    proj = make_project(tmp_path)
    write(proj, "a.md", f"```\n{link(OLD)}\n```\n")
    r = run(proj, no_git)
    assert r.returncode == 1
    assert "a.md:2" in r.stderr


# Characters that can follow a URL in Markdown or prose. Each ends the ref.
# "." and "_" end it only after a full 40-hex hash: a branch name can hold them.
FOLLOWERS = ["`", "]", "}", ",", ";", ":", ".", "*", "_", "|", "\\", "'"]


@pytest.mark.parametrize("c", FOLLOWERS)
def test_hash_ends_at_markdown_followers(tmp_path, no_git, c):
    proj = make_project(tmp_path)
    base = "https://github.com/RobCo-Industries/RobCo-Core/blob/"
    write(proj, "a.md", f"see {base}{OLD}{c} now\n")
    r = run(proj, no_git)
    assert r.returncode == 1, r.stderr
    assert f"a.md:1 links RobCo-Core at {OLD}," in r.stderr
    write(proj, "a.md", f"see {base}{COMMIT}{c} now\n{base}{COMMIT}{c}\n")
    r = run(proj, no_git)
    assert r.returncode == 0, r.stderr
    write(proj, "a.md", f"see {base}main{c} now\n{base}release-1{c}\n")
    r = run(proj, no_git)
    assert r.returncode == 0, r.stderr


def test_inline_code_pin(tmp_path, no_git):
    proj = make_project(tmp_path)
    write(proj, "a.md", f"Pinned at `{link(OLD, path='')}` and `https://github.com/RobCo-Industries/RobCo-Core/blob/{OLD}`.\n")
    r = run(proj, no_git)
    assert r.returncode == 1
    assert r.stderr.count("links RobCo-Core") == 2, r.stderr


def test_dotted_and_underscored_branch_names_are_ignored(tmp_path, no_git):
    proj = make_project(tmp_path)
    base = "https://github.com/RobCo-Industries/RobCo-Core/tree/"
    write(proj, "a.md", f"{base}v1.0.2 {base}my_branch {base}{OLD[:20]}_x {base}{OLD}.x\n")
    assert run(proj, no_git).returncode == 0


# More than one pinned repository: --repo and nested submodules.

PIPOS = "RobCo-Industries/Pip-OS"
OUTER = "external/pip-os"
INNER = OUTER + "/external/robco-core"
PIPOS_COMMIT = "3" * 40
PIPOS_OLD = "4" * 40


def repo_link(repo, commit, path="docs/x.md"):
    return f"https://github.com/{repo}/blob/{commit}/{path}"


def run_repo(proj, env, repo, sub, *args):
    return subprocess.run(["bash", str(SCRIPT), "--repo", repo, str(proj), sub, *args],
                          capture_output=True, text=True, env=env)


def make_nested(tmp_path):
    """A project pinning Pip-OS, which pins RobCo-Core, laid out as git does.

    The outer .git file points at .git/modules/pip-os. The inner one points at
    .git/modules/pip-os/modules/robco-core, inside the parent's modules folder.
    """
    proj = tmp_path / "proj"
    outer_git = proj / ".git" / "modules" / "pip-os"
    inner_git = outer_git / "modules" / "robco-core"
    inner_git.mkdir(parents=True)
    (proj / INNER).mkdir(parents=True)
    (proj / OUTER / ".git").write_text("gitdir: ../../.git/modules/pip-os\n")
    (proj / INNER / ".git").write_text("gitdir: ../../../../.git/modules/pip-os/modules/robco-core\n")
    (outer_git / "HEAD").write_text(PIPOS_COMMIT + "\n")
    (inner_git / "HEAD").write_text(COMMIT + "\n")
    return proj


def test_other_repo_name(tmp_path, no_git):
    proj = make_project(tmp_path)
    write(proj, "a.md", f"[ok]({repo_link(PIPOS, COMMIT)})\n[core]({link(OLD)})\n"
          f"[old]({repo_link(PIPOS, OLD)})\n")
    r = run_repo(proj, no_git, PIPOS, SUB)
    assert r.returncode == 1
    assert f"a.md:3 links Pip-OS at {OLD}, the submodule is at {COMMIT}" in r.stderr
    assert "a.md:2" not in r.stderr


def test_repo_dot_is_literal(tmp_path, no_git):
    proj = make_project(tmp_path)
    write(proj, "a.md", f"[x]({repo_link('o/a.b', OLD)})\n")
    assert run_repo(proj, no_git, "o/axb", SUB).returncode == 0
    r = run_repo(proj, no_git, "o/a.b", SUB)
    assert r.returncode == 1, r.stdout


def test_bad_repo_is_usage_error(tmp_path, no_git):
    proj = make_project(tmp_path)
    for bad in ("RobCo-Core", "a/b/c", "a/b c", ""):
        assert run_repo(proj, no_git, bad, SUB).returncode == 2


def test_nested_submodule_commit(tmp_path, no_git):
    proj = make_nested(tmp_path)
    write(proj, "README.md", f"[core]({link(COMMIT)})\n")
    r = run(proj, no_git, sub=INNER)
    assert r.returncode == 0, r.stderr
    assert f"ok ({COMMIT})" in r.stdout


def test_nested_skips_outer_submodule(tmp_path, no_git):
    proj = make_nested(tmp_path)
    write(proj, OUTER + "/docs/x.md", f"[old]({link(OLD)})\n")
    write(proj, "external/notes.md", f"[core]({link(COMMIT)})\n")
    r = run(proj, no_git, sub=INNER)
    assert r.returncode == 0, r.stderr


def test_two_repos_one_stale_link_each(tmp_path, no_git):
    proj = make_nested(tmp_path)
    write(proj, "docs/a.md", f"[p]({repo_link(PIPOS, PIPOS_COMMIT)})\n[p]({repo_link(PIPOS, PIPOS_OLD)})\n")
    write(proj, "docs/b.md", f"[c]({link(COMMIT)})\n[c]({link(OLD)})\n")
    p = run_repo(proj, no_git, PIPOS, OUTER)
    c = run(proj, no_git, sub=INNER)
    assert p.returncode == 1 and c.returncode == 1
    assert f"docs/a.md:2 links Pip-OS at {PIPOS_OLD}, the submodule is at {PIPOS_COMMIT}" in p.stderr
    assert "docs/b.md" not in p.stderr
    assert f"docs/b.md:2 links RobCo-Core at {OLD}, the submodule is at {COMMIT}" in c.stderr
    assert "docs/a.md" not in c.stderr


def test_two_argument_form_unchanged(tmp_path, no_git):
    proj = make_project(tmp_path)
    write(proj, "a.md", f"[old]({link(OLD)})\n")
    r = subprocess.run(["bash", str(SCRIPT), str(proj), SUB], capture_output=True, text=True, env=no_git)
    assert r.returncode == 1
    assert f"a.md:1 links RobCo-Core at {OLD}, the submodule is at {COMMIT}" in r.stderr
