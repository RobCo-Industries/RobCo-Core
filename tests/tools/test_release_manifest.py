"""tools/release/manifest.py on stand-in checkouts made in tmp_path."""
import json
import os
import subprocess
import sys

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
TOOL = os.path.join(ROOT, "tools", "release", "manifest.py")
PLAN = os.path.join(ROOT, "tools", "nightly", "plan.py")
GIT_ENV = {
    "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@example.invalid",
    "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@example.invalid",
    "GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1",
    "PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": os.devnull,
}
BRANCH = {"RobCo-Core": "main", "RETROS-BIOS": "main", "MFBootAgent": "master",
          "UOS": "master", "Pip-OS": "master", "Deitrix": "master"}
NAMES = tuple(BRANCH)
CHANGELOG = "# Changelog\n\n## [Unreleased]\n\n## [{v}] - 2026-12-01\n\n- First.\n"


def header(prefix, version="0.1.0"):
    major, minor, patch = version.split(".")
    return (f"#define {prefix}_VERSION_MAJOR {major}\n#define {prefix}_VERSION_MINOR {minor}\n"
            f"#define {prefix}_VERSION_PATCH {patch}\n"
            f'#define {prefix}_VERSION_STR "{version}"\n')


CODE = {
    "RobCo-Core": {},
    "RETROS-BIOS": {"src/robco_config.h": header("RETROS")},
    "MFBootAgent": {"src/robco_config.h": header("MFB")},
    "UOS": {"src/kernel/robco_config.h": header("UOS")},
    "Pip-OS": {"kernel/robco_config.h": header("PIPOS", "7.1.0")},
    "Deitrix": {"Makefile": "NAME := deitrix\nVERSION      := 0.1.0\n"},
}
VERSION = {n: ("7.1.0" if n == "Pip-OS" else "0.1.0") for n in NAMES}
PLAN_FILES = {"pytest.ini": "[pytest]\nrobco_boards = raspi0 raspi2b\n",
              ".github/workflows/ci.yml": "name: ci\n"}


def git(repo, *args, stdin=None):
    return subprocess.run(["git", "-C", str(repo), *args], check=True, env=GIT_ENV,
                          capture_output=True, text=True, input=stdin).stdout.strip()


def commit(repo, files=None, links=None, branch="main"):
    repo.mkdir(parents=True, exist_ok=True)
    if not (repo / ".git").exists():
        git(repo, "init", "-q", "-b", branch)
    for path, text in (files or {}).items():
        (repo / path).parent.mkdir(parents=True, exist_ok=True)
        (repo / path).write_text(text)
        git(repo, "add", path)
    for path, sha in (links or {}).items():
        git(repo, "update-index", "--add", "--cacheinfo", f"160000,{sha},{path}")
    git(repo, "commit", "-q", "--allow-empty", "-m", "c")
    return git(repo, "rev-parse", "HEAD")


def core_copy(folder):
    """The files plan.py reads from a sibling's library submodule."""
    (folder / "ci").mkdir(parents=True, exist_ok=True)
    (folder / "ci" / "IMAGE_TAG").write_text("abc123def456\n")
    script = folder / "tools" / "check_pins.sh"
    script.parent.mkdir(parents=True, exist_ok=True)
    script.write_text("#!/bin/sh\nexit 0\n")
    script.chmod(0o755)


@pytest.fixture
def work(tmp_path):
    """Six stand-in checkouts whose pins line up, and a manifest of them."""
    w = tmp_path / "w"
    shas = {}

    def files(name):
        return {**CODE[name], **PLAN_FILES, "CHANGELOG.md": CHANGELOG.format(v=VERSION[name])}

    shas["RobCo-Core"] = commit(w / "RobCo-Core", {**files("RobCo-Core"),
                                                   "ci/IMAGE_TAG": "abc123def456\n"})
    for name in ("RETROS-BIOS", "MFBootAgent", "UOS", "Pip-OS"):
        shas[name] = commit(w / name, files(name), {"external/robco-core": shas["RobCo-Core"]},
                            BRANCH[name])
        core_copy(w / name / "external" / "robco-core")
    shas["Deitrix"] = commit(w / "Deitrix", files("Deitrix"),
                             {"external/pip-os": shas["Pip-OS"]}, "master")
    inner = w / "Deitrix" / "external" / "pip-os"
    commit(inner, links={"external/robco-core": shas["RobCo-Core"]})
    core_copy(inner / "external" / "robco-core")
    manifest = tmp_path / "suite-2026.12.json"
    write(manifest, {"suite": "suite-2026.12", "repositories": {
        n: {"version": VERSION[n], "commit": shas[n]} for n in NAMES}})
    return w, shas, manifest


def write(path, data):
    path.write_text(json.dumps(data, indent=2) + "\n")


def edit(manifest, change):
    data = json.loads(manifest.read_text())
    change(data)
    write(manifest, data)


def tool(*args):
    proc = subprocess.run([sys.executable, "-I", TOOL, *map(str, args)],
                          capture_output=True, text=True, env=GIT_ENV)
    return proc.returncode, proc.stdout


def check(w, manifest, *extra):
    return tool("check", "--manifest", manifest, "--work", w, *extra)


def errors(out):
    return [line for line in out.splitlines() if line.startswith("::error::")]


# new

def test_new_writes_the_six_commits(work, tmp_path):
    w, shas, _ = work
    out = tmp_path / "new.json"
    code, _ = tool("new", "--suite", "suite-2026.12", "--work", w, "--out", out)
    assert code == 0
    data = json.loads(out.read_text())
    assert data["suite"] == "suite-2026.12"
    assert {n: e["commit"] for n, e in data["repositories"].items()} == shas
    assert data["repositories"]["Pip-OS"]["version"] == "7.1.0"
    assert data["repositories"]["RobCo-Core"]["version"] == "0.1.0"
    assert check(w, out)[0] == 0


def test_new_refuses_a_bad_suite_name(work, tmp_path):
    w, _, _ = work
    code, out = tool("new", "--suite", "pipos-1.5.0", "--work", w, "--out", tmp_path / "x")
    assert code == 1 and errors(out)
    assert not (tmp_path / "x").exists()


# check: happy path and outputs

def test_check_passes_and_writes_outputs(work, tmp_path):
    w, shas, manifest = work
    gh = tmp_path / "gh"
    code, out = check(w, manifest, "--tag", "suite-2026.12", "--github-output", gh)
    assert code == 0, out
    lines = gh.read_text().splitlines()
    assert "suite=suite-2026.12" in lines
    assert f"core_sha={shas['RobCo-Core']}" in lines
    assert f"deitrix_sha={shas['Deitrix']}" in lines
    assert len(lines) == 7


# check: one test per row of the fail-closed table

def test_unknown_key_short_commit_and_missing_repository(work):
    w, _, manifest = work

    def change(d):
        d["extra"] = 1
        d["repositories"]["UOS"]["commit"] = d["repositories"]["UOS"]["commit"][:12]
        del d["repositories"]["Deitrix"]
    edit(manifest, change)
    code, out = check(w, manifest)
    assert code == 1
    text = "\n".join(errors(out))
    assert "unknown key 'extra'" in text
    assert "UOS: commit" in text
    assert "Deitrix is missing" in text


def test_commit_not_on_default_branch(work):
    w, _, manifest = work
    repo = w / "MFBootAgent"
    git(repo, "checkout", "-q", "-b", "side")
    side = commit(repo)
    git(repo, "checkout", "-q", "master")
    git(repo, "branch", "-q", "-D", "side")
    git(repo, "checkout", "-q", "--detach", side)
    edit(manifest, lambda d: d["repositories"]["MFBootAgent"].update(commit=side))
    code, out = check(w, manifest)
    assert code == 1
    assert errors(out) == [f"::error::manifest: MFBootAgent: {side[:12]} is not on master"]


def test_library_pin_and_deitrix_pin_differ(work):
    w, shas, manifest = work
    newer = commit(w / "RobCo-Core")
    pipos = commit(w / "Pip-OS", branch="master")

    def change(d):
        d["repositories"]["RobCo-Core"]["commit"] = newer
        d["repositories"]["Pip-OS"]["commit"] = pipos
    edit(manifest, change)
    code, out = check(w, manifest)
    assert code == 1
    text = errors(out)
    for name in ("RETROS-BIOS", "MFBootAgent", "UOS", "Pip-OS"):
        assert any(f"{name}: pins RobCo-Core" in e for e in text)
    assert any("Deitrix: pins Pip-OS" in e for e in text)
    assert len(text) == 5


@pytest.mark.parametrize("log", [
    "# Changelog\n\n## Unreleased\n\n- Pending.\n\n## 0.1.0\n",
    "# Changelog\n\n## [Unreleased]\n\n## [0.0.9] - 2026-11-01\n",
    "# Changelog\n\n## [0.1.0]\n",
])
def test_changelog_rule_and_rehearsal(work, log):
    w, _, manifest = work
    commit(w / "UOS", {"CHANGELOG.md": log}, branch="master")
    edit(manifest, lambda d: d["repositories"]["UOS"].update(
        commit=git(w / "UOS", "rev-parse", "HEAD")))
    code, out = check(w, manifest)
    assert code == 1
    assert errors(out) and all("UOS: CHANGELOG.md" in e for e in errors(out))
    assert check(w, manifest, "--rehearsal")[0] == 0


def test_changelog_takes_both_heading_forms(work):
    w, _, manifest = work
    commit(w / "UOS", {"CHANGELOG.md": "# Changelog\n\n## Unreleased\n\n## 0.1.0\n\n- x\n"},
           branch="master")
    edit(manifest, lambda d: d["repositories"]["UOS"].update(
        commit=git(w / "UOS", "rev-parse", "HEAD")))
    assert check(w, manifest)[0] == 0


def test_version_in_code_differs(work):
    w, _, manifest = work

    def change(d):
        for name in ("RETROS-BIOS", "MFBootAgent", "UOS", "Pip-OS", "Deitrix"):
            d["repositories"][name]["version"] = "0.2.0"
    edit(manifest, change)
    code, out = check(w, manifest, "--rehearsal")
    assert code == 1
    assert len(errors(out)) == 5
    assert all("the code says" in e for e in errors(out))


def test_pushed_tag_is_not_the_suite(work):
    w, _, manifest = work
    code, out = check(w, manifest, "--tag", "suite-2027.01")
    assert code == 1
    assert errors(out) == ["::error::manifest: the tag suite-2027.01 is not the manifest's "
                           "suite 'suite-2026.12'"]


def test_check_lists_every_problem(work):
    w, _, manifest = work
    edit(manifest, lambda d: d["repositories"]["Deitrix"].update(version="9.9.9"))
    code, out = check(w, manifest, "--tag", "suite-2027.01")
    assert code == 1
    text = "\n".join(errors(out))
    assert "the tag suite-2027.01" in text
    assert "Deitrix: the code says 0.1.0" in text
    assert "Deitrix: CHANGELOG.md" in text


def test_checkout_at_another_commit(work):
    w, _, manifest = work
    commit(w / "RETROS-BIOS", branch="main")
    code, out = check(w, manifest)
    assert code == 1
    assert any("RETROS-BIOS: the checkout is at" in e for e in errors(out))


# v tags on the remotes

def remotes(w):
    return [f"--remote={n}={w / n}" for n in NAMES]


def lightweight(repo, tag, sha):
    git(repo, "update-ref", f"refs/tags/{tag}", sha)


def test_remote_tags_missing_is_printed_or_required(work):
    w, _, manifest = work
    code, out = tool("remote-tags", "--manifest", manifest, *remotes(w))
    assert code == 0
    assert out.count("is missing") == 6
    code, out = tool("remote-tags", "--manifest", manifest, "--require", *remotes(w))
    assert code == 1 and len(errors(out)) == 6


def test_remote_tags_present_and_on_another_commit(work):
    w, shas, manifest = work
    for name in NAMES:
        lightweight(w / name, f"v{VERSION[name]}", shas[name])
    # An annotated tag is read through its peeled commit.
    obj = git(w / "UOS", "mktag", stdin=(
        f"object {shas['UOS']}\ntype commit\ntag v0.1.0\n"
        "tagger t <t@example.invalid> 0 +0000\n\nUOS v0.1.0\n"))
    git(w / "UOS", "update-ref", "refs/tags/v0.1.0", obj)
    code, out = tool("remote-tags", "--manifest", manifest, "--require", *remotes(w))
    assert code == 0, out
    old = git(w / "Deitrix", "rev-parse", "HEAD~0")
    newer = commit(w / "Deitrix", branch="master")
    lightweight(w / "Deitrix", "v0.1.0", newer)
    code, out = tool("remote-tags", "--manifest", manifest, *remotes(w))
    assert code == 1
    assert errors(out) == [f"::error::manifest: Deitrix: v0.1.0 is on {newer[:12]}, "
                           f"the manifest names {old[:12]}"]


def test_remote_tags_on_a_bare_remote(work, tmp_path):
    w, shas, manifest = work
    bare = tmp_path / "bare.git"
    subprocess.run(["git", "init", "-q", "--bare", str(bare)], check=True, env=GIT_ENV)
    # Borrow the stand-in's objects, then point the tag at its commit.
    (bare / "objects" / "info" / "alternates").write_text(str(w / "UOS" / ".git" / "objects"))
    git(bare, "update-ref", "refs/tags/v0.1.0", shas["UOS"])
    args = [a for a in remotes(w) if not a.startswith("--remote=UOS=")]
    code, out = tool("remote-tags", "--manifest", manifest, *args, f"--remote=UOS={bare}")
    assert code == 0
    assert f"UOS: v0.1.0 is on {shas['UOS'][:12]}" in out


def test_remote_tags_unreachable_remote_fails(work, tmp_path):
    w, _, manifest = work
    args = [a for a in remotes(w) if not a.startswith("--remote=UOS=")]
    code, out = tool("remote-tags", "--manifest", manifest, *args,
                     f"--remote=UOS={tmp_path / 'none'}")
    assert code == 1 and any("UOS: git ls-remote" in e for e in errors(out))


# tags

def test_tags_prints_and_runs_nothing(work):
    w, shas, manifest = work
    code, out = tool("tags", "--manifest", manifest)
    assert code == 0
    lines = out.splitlines()
    assert lines[0] == f'git -C RobCo-Core tag -a v0.1.0 -m "RobCo-Core v0.1.0" {shas["RobCo-Core"]}'
    assert f'git -C Pip-OS tag -a v7.1.0 -m "Pip-OS v7.1.0" {shas["Pip-OS"]}' in lines
    assert lines[-2] == 'git -C RobCo-Core tag -a suite-2026.12 -m "suite-2026.12" MANIFEST_COMMIT'
    assert lines[-1] == "git -C RobCo-Core push origin refs/tags/suite-2026.12"
    assert sum(" tag -a " in line for line in lines) == 7
    assert not any("pipos-" in line for line in lines)
    for name in NAMES:
        assert git(w / name, "tag", "--list") == ""


# verify-night

def night(tmp_path, w):
    versions = tmp_path / "VERSIONS.txt"
    proc = subprocess.run(
        [sys.executable, "-I", PLAN, "--work", str(w), "--pairing", "pins",
         "--github-output", str(tmp_path / "plan-out"), "--summary", str(tmp_path / "sum.md"),
         "--versions", str(versions)], capture_output=True, text=True, env=GIT_ENV)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    return versions


def test_verify_night_on_a_real_plan(work, tmp_path):
    w, _, manifest = work
    versions = night(tmp_path, w)
    code, out = tool("verify-night", "--manifest", manifest, "--versions", versions)
    assert code == 0, out


def test_verify_night_other_commit_and_note(work, tmp_path):
    w, shas, manifest = work
    pipos = commit(w / "Pip-OS", branch="master")
    versions = night(tmp_path, w)
    code, out = tool("verify-night", "--manifest", manifest, "--versions", versions)
    assert code == 1
    text = errors(out)
    assert f"::error::manifest: Pip-OS: the night built '{pipos[:12]}', the manifest names " \
           f"{shas['Pip-OS'][:12]}" in text
    assert any("Deitrix: the night notes a pin that differs" in e for e in text)
    assert len(text) == 2


def test_verify_night_missing_row(work, tmp_path):
    w, _, manifest = work
    versions = night(tmp_path, w)
    text = "\n".join(l for l in versions.read_text().splitlines() if not l.startswith("| UOS "))
    versions.write_text(text)
    code, out = tool("verify-night", "--manifest", manifest, "--versions", versions)
    assert code == 1
    assert errors(out) == ["::error::manifest: UOS: no row in VERSIONS.txt"]
