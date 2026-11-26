"""tools/nightly/plan.py on a small fake work folder of git repositories."""
import json
import os
import subprocess
import sys

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
PLAN = os.path.join(ROOT, "tools", "nightly", "plan.py")
GIT_ENV = {
    "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@example.invalid",
    "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@example.invalid",
    "GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1",
    "PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": os.devnull,
}
BOARDS = "raspi0 raspi1ap raspi2b raspi3b"


def git(repo, *args):
    return subprocess.run(["git", "-C", str(repo), *args], check=True, env=GIT_ENV,
                          capture_output=True, text=True).stdout.strip()


def commit(repo, files=None, links=None, message="c"):
    """Commit files ({path: text}) and gitlinks ({path: sha}) in repo."""
    repo.mkdir(parents=True, exist_ok=True)
    if not (repo / ".git").exists():
        git(repo, "init", "-q", "-b", "main")
    for path, text in (files or {}).items():
        (repo / path).parent.mkdir(parents=True, exist_ok=True)
        (repo / path).write_text(text)
        git(repo, "add", path)
    for path, sha in (links or {}).items():
        git(repo, "update-index", "--add", "--cacheinfo", f"160000,{sha},{path}")
    git(repo, "commit", "-q", "--allow-empty", "-m", message)
    return git(repo, "rev-parse", "HEAD")


def core_copy(folder, tag="abc123def456", pins_ok=True):
    """The files a sibling reads from its RobCo-Core submodule."""
    (folder / "ci").mkdir(parents=True, exist_ok=True)
    (folder / "ci" / "IMAGE_TAG").write_text(tag + "\n")
    script = folder / "tools" / "check_pins.sh"
    script.parent.mkdir(parents=True, exist_ok=True)
    script.write_text("#!/bin/sh\n" + ("exit 0\n" if pins_ok else
                                       "echo 'check-pins: tag differs' >&2\nexit 1\n"))
    script.chmod(0o755)


SIBLING_FILES = {"pytest.ini": f"[pytest]\nrobco_boards = {BOARDS}\n",
                 ".github/workflows/ci.yml": "name: ci\n"}


@pytest.fixture
def work(tmp_path):
    w = tmp_path / "w"
    core = w / "RobCo-Core"
    first = commit(core, {"ci/IMAGE_TAG": "abc123def456\n"})
    commit(core)
    commit(core)
    shas = {"core-first": first}
    for name in ("RETROS-BIOS", "MFBootAgent", "UOS", "Pip-OS"):
        shas[name] = commit(w / name, SIBLING_FILES, {"external/robco-core": first})
        core_copy(w / name / "external" / "robco-core")
    pipos_old = git(w / "Pip-OS", "rev-parse", "HEAD")
    shas["Pip-OS"] = commit(w / "Pip-OS")
    shas["pipos-old"] = pipos_old
    shas["Deitrix"] = commit(w / "Deitrix", SIBLING_FILES, {"external/pip-os": pipos_old})
    inner = w / "Deitrix" / "external" / "pip-os"
    commit(inner, links={"external/robco-core": first})
    core_copy(inner / "external" / "robco-core")
    return w, shas


def run_plan(tmp_path, w, pairing="pins", env=None):
    out, summary, versions = (tmp_path / n for n in ("out", "summary.md", "VERSIONS.txt"))
    proc = subprocess.run(
        [sys.executable, "-I", PLAN, "--work", str(w), "--pairing", pairing,
         "--github-output", str(out), "--summary", str(summary), "--versions", str(versions)],
        capture_output=True, text=True, env={**GIT_ENV, **(env or {})})
    outputs = {}
    if out.exists():
        for line in out.read_text().splitlines():
            key, _, value = line.partition("=")
            outputs[key] = json.loads(value)
    text = summary.read_text() if summary.exists() else ""
    return proc, outputs, text, versions


def test_plan_writes_six_matrices(tmp_path, work):
    w, shas = work
    proc, outputs, summary, versions = run_plan(tmp_path, w)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert sorted(outputs) == sorted(["stages", "card_terminal", "card_pipboy",
                                      "agent", "uos", "pipboy"])
    stages = outputs["stages"]["include"]
    assert [e["repo"] for e in stages] == ["RETROS-BIOS", "MFBootAgent"]
    assert stages[0] == {"pairing": "pins", "repo": "RETROS-BIOS", "sha": shas["RETROS-BIOS"],
                         "core_sha": "", "pipos_sha": "", "tag": "abc123def456"}
    assert outputs["card_terminal"]["include"][0]["sha"] == shas["UOS"]
    pip = outputs["card_pipboy"]["include"][0]
    assert (pip["repo"], pip["sha"], pip["pipos_sha"]) == ("Deitrix", shas["Deitrix"], shas["Pip-OS"])
    for key, repo in (("agent", "MFBootAgent"), ("uos", "UOS"), ("pipboy", "Deitrix")):
        entries = outputs[key]["include"]
        assert [e["board"] for e in entries] == BOARDS.split()
        assert {e["repo"] for e in entries} == {repo}
    assert versions.read_text() in summary


def test_table_counts_commits_behind_main(tmp_path, work):
    w, shas = work
    proc, _, summary, _ = run_plan(tmp_path, w, env={"NIGHTLY_REF_UOS": "my-branch"})
    assert proc.returncode == 0, proc.stdout + proc.stderr
    rows = {line.split("|")[1].strip(): [c.strip() for c in line.split("|")[2:-1]]
            for line in summary.splitlines() if line.startswith("| ") and "---" not in line}
    assert rows["RobCo-Core"][3] == "0"
    assert rows["UOS"] == ["`my-branch`", f"`{shas['UOS'][:12]}`",
                           f"`{shas['core-first'][:12]}`", "2", "`abc123def456`"]
    assert rows["RETROS-BIOS"][0] == "`default branch`"
    assert f"Deitrix pins Pip-OS `{shas['pipos-old'][:12]}`, 1 commit behind" in summary


def test_both_plans_pins_only(tmp_path, work):
    proc, outputs, summary, _ = run_plan(tmp_path, work[0], pairing="both")
    assert proc.returncode == 0
    assert {e["pairing"] for e in outputs["stages"]["include"]} == {"pins"}
    assert "core-head` is not built yet" in summary


def test_core_head_fails(tmp_path, work):
    proc, outputs, _, _ = run_plan(tmp_path, work[0], pairing="core-head")
    assert proc.returncode == 1
    assert outputs == {}
    assert "not built yet" in proc.stdout


def test_pin_mismatch_fails_and_names_the_sibling(tmp_path, work):
    w, _ = work
    core_copy(w / "UOS" / "external" / "robco-core", pins_ok=False)
    proc, outputs, _, _ = run_plan(tmp_path, w)
    assert proc.returncode == 1
    assert outputs == {}
    assert "::error::plan: UOS: check_pins.sh failed: check-pins: tag differs" in proc.stdout


def test_every_problem_is_listed(tmp_path, work):
    w, _ = work
    os.remove(w / "RETROS-BIOS" / "external" / "robco-core" / "ci" / "IMAGE_TAG")
    os.remove(w / "Deitrix" / "external" / "pip-os" / "external" / "robco-core"
              / "tools" / "check_pins.sh")
    proc, _, _, _ = run_plan(tmp_path, w)
    assert proc.returncode == 1
    assert "RETROS-BIOS: cannot read" in proc.stdout
    assert "Deitrix:" in proc.stdout and "check_pins.sh is missing" in proc.stdout


def test_missing_checkout_fails(tmp_path, work):
    w, _ = work
    os.rename(w / "MFBootAgent", tmp_path / "gone")
    proc, _, _, _ = run_plan(tmp_path, w)
    assert proc.returncode == 1
    assert "MFBootAgent: git rev-parse HEAD failed" in proc.stdout


def test_missing_boards_fails(tmp_path, work):
    w, _ = work
    (w / "UOS" / "pytest.ini").write_text("[pytest]\n")
    proc, _, _, _ = run_plan(tmp_path, w)
    assert proc.returncode != 0
    assert "has no robco_boards line" in proc.stdout + proc.stderr


def test_empty_image_tag_fails(tmp_path, work):
    w, _ = work
    (w / "Pip-OS" / "external" / "robco-core" / "ci" / "IMAGE_TAG").write_text("\n")
    proc, _, _, _ = run_plan(tmp_path, w)
    assert proc.returncode == 1
    assert "IMAGE_TAG is empty" in proc.stdout
