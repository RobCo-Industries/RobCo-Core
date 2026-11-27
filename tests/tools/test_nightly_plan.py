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
                         "core_sha": shas["core-first"], "pipos_sha": "", "tag": "abc123def456"}
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
    assert rows["RobCo-Core"][4] == "0"
    assert rows["UOS"] == ["`my-branch`", f"`{shas['UOS'][:12]}`",
                           f"`{shas['core-first'][:12]}`", "none", "2", "`abc123def456`", ""]
    assert rows["RETROS-BIOS"][0] == "`default branch`"
    assert f"Deitrix pins Pip-OS `{shas['pipos-old'][:12]}`, 1 commit behind" in summary


def head_tag(w, tag):
    """Commit a new ci/IMAGE_TAG on the library head. Returns the head."""
    return commit(w / "RobCo-Core", {"ci/IMAGE_TAG": tag + "\n"}, message="tag")


def test_core_head_builds_on_the_library_head(tmp_path, work):
    w, shas = work
    head = head_tag(w, "headtag00000")
    proc, outputs, summary, _ = run_plan(tmp_path, w, pairing="core-head")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert sorted(outputs) == sorted(["stages", "card_terminal", "card_pipboy",
                                      "agent", "uos", "pipboy"])
    entries = [e for m in outputs.values() for e in m["include"]]
    assert {(e["pairing"], e["core_sha"], e["tag"]) for e in entries} == {
        ("core-head", head, "headtag00000")}
    assert outputs["stages"]["include"][0] == {
        "pairing": "core-head", "repo": "RETROS-BIOS", "sha": shas["RETROS-BIOS"],
        "core_sha": head, "pipos_sha": "", "tag": "headtag00000"}
    pip = outputs["card_pipboy"]["include"][0]
    assert (pip["sha"], pip["pipos_sha"]) == (shas["Deitrix"], shas["Pip-OS"])
    for key in ("agent", "uos", "pipboy"):
        assert [e["board"] for e in outputs[key]["include"]] == ["raspi0", "raspi2b"]
    assert "Pairings planned: `core-head`." in summary
    assert "not built yet" not in summary + proc.stdout


def test_core_head_keeps_only_the_boards_a_sibling_lists(tmp_path, work):
    w, _ = work
    (w / "UOS" / "pytest.ini").write_text("[pytest]\nrobco_boards = raspi1ap raspi2b\n")
    (w / "Deitrix" / "pytest.ini").write_text("[pytest]\nrobco_boards = raspi0 raspi3b\n")
    proc, outputs, _, _ = run_plan(tmp_path, w, pairing="core-head")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert [e["board"] for e in outputs["uos"]["include"]] == ["raspi2b"]
    assert [e["board"] for e in outputs["pipboy"]["include"]] == ["raspi0"]
    assert [e["board"] for e in outputs["agent"]["include"]] == ["raspi0", "raspi2b"]


def test_both_holds_pins_then_core_head(tmp_path, work):
    w, shas = work
    head = head_tag(w, "headtag00000")
    (tmp_path / "p").mkdir()
    (tmp_path / "c").mkdir()
    _, pins, _, _ = run_plan(tmp_path / "p", w, pairing="pins")
    _, core_head, _, _ = run_plan(tmp_path / "c", w, pairing="core-head")
    proc, outputs, summary, _ = run_plan(tmp_path, w, pairing="both")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    for key, matrix in outputs.items():
        assert matrix["include"] == pins[key]["include"] + core_head[key]["include"]
    assert outputs["stages"]["include"][0]["core_sha"] == shas["core-first"]
    assert outputs["stages"]["include"][2]["core_sha"] == head
    assert "Pairings planned: `pins`, `core-head`." in summary


@pytest.mark.parametrize("pairing", ["core-head", "both"])
def test_table_names_the_core_head_library_and_image(tmp_path, work, pairing):
    w, _ = work
    head = head_tag(w, "headtag00000")
    proc, _, summary, versions = run_plan(tmp_path, w, pairing=pairing)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    line = (f"In `core-head` every sibling builds on RobCo-Core `{head[:12]}`, "
            "in the image `headtag00000`. The Library and Image tag columns show "
            "the `pins` side.")
    assert line in summary
    assert line in versions.read_text()


def test_pins_table_has_no_core_head_line(tmp_path, work):
    w, _ = work
    head_tag(w, "headtag00000")
    proc, _, summary, versions = run_plan(tmp_path, w, pairing="pins")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "In `core-head`" not in summary
    assert "In `core-head`" not in versions.read_text()


def test_pins_is_unchanged_by_the_library_head(tmp_path, work):
    w, shas = work
    head_tag(w, "headtag00000")
    proc, outputs, summary, _ = run_plan(tmp_path, w, pairing="pins")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    entries = [e for m in outputs.values() for e in m["include"]]
    assert {(e["pairing"], e["core_sha"], e["tag"]) for e in entries} == {
        ("pins", shas["core-first"], "abc123def456")}
    assert [e["board"] for e in outputs["uos"]["include"]] == BOARDS.split()
    assert "Pairings planned: `pins`." in summary


@pytest.mark.parametrize("pairing", ["pins", "both"])
def test_pin_mismatch_fails_and_names_the_sibling(tmp_path, work, pairing):
    w, _ = work
    core_copy(w / "UOS" / "external" / "robco-core", pins_ok=False)
    proc, outputs, _, _ = run_plan(tmp_path, w, pairing=pairing)
    assert proc.returncode == 1
    assert outputs == {}
    assert "::error::plan: UOS: check_pins.sh failed: check-pins: tag differs" in proc.stdout


def test_core_head_alone_skips_the_pin_check(tmp_path, work):
    w, _ = work
    core_copy(w / "UOS" / "external" / "robco-core", pins_ok=False)
    proc, outputs, _, _ = run_plan(tmp_path, w, pairing="core-head")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "check_pins" not in proc.stdout
    assert outputs["uos"]["include"]


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


def scenario_work(tmp_path, case):
    """A fake work folder for one row of the version table test.

    Returns the folder and, per repository, the (commit, library, Pip-OS,
    tag) the nightly builds it with."""
    w = tmp_path / "w"
    core = w / "RobCo-Core"
    old = commit(core, {"ci/IMAGE_TAG": "tagold000000\n"})
    new = commit(core, {"ci/IMAGE_TAG": "tagnew000000\n"})
    built = {"RobCo-Core": (new, "", "", "tagnew000000")}
    for name in ("RETROS-BIOS", "MFBootAgent", "UOS"):
        tag = "tagown000000" if case == "sibling-own-tag" and name == "UOS" else "tagold000000"
        sha = commit(w / name, SIBLING_FILES, {"external/robco-core": old})
        core_copy(w / name / "external" / "robco-core", tag)
        built[name] = (sha, old, "", tag)
    pipos = w / "Pip-OS"
    pinned_pipos = commit(pipos, SIBLING_FILES, {"external/robco-core": old})
    head_core, head_tag = old, "tagold000000"
    if case in ("deitrix-behind", "pipos-newer-core"):
        if case == "pipos-newer-core":
            head_core, head_tag = new, "tagnew000000"
        commit(pipos, links={"external/robco-core": head_core}, message="head")
    core_copy(pipos / "external" / "robco-core", head_tag)
    head = git(pipos, "rev-parse", "HEAD")
    built["Pip-OS"] = (head, head_core, "", head_tag)
    deitrix = commit(w / "Deitrix", SIBLING_FILES, {"external/pip-os": pinned_pipos})
    inner = w / "Deitrix" / "external" / "pip-os"
    commit(inner, links={"external/robco-core": old})
    core_copy(inner / "external" / "robco-core", "tagold000000")
    built["Deitrix"] = (deitrix, head_core, head, head_tag)
    return w, built, pinned_pipos


def summary_rows(text):
    return {line.split("|")[1].strip(): [c.strip() for c in line.split("|")[2:-1]]
            for line in text.splitlines()
            if line.startswith("| ") and "---" not in line and "Repository" not in line}


@pytest.mark.parametrize("case", ["at-pins", "deitrix-behind", "pipos-newer-core",
                                  "sibling-own-tag"])
def test_matrix_summary_and_versions_agree(tmp_path, case):
    w, built, pinned_pipos = scenario_work(tmp_path, case)
    proc, outputs, summary, versions = run_plan(tmp_path, w)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    from_summary = summary_rows(summary)
    from_versions = summary_rows(versions.read_text())
    entries = [e for m in outputs.values() for e in m["include"]]
    for repo, (sha, core, pipos, tag) in built.items():
        short = [f"`{x[:12]}`" if x else "none" for x in (sha, core, pipos)]
        for rows in (from_summary, from_versions):
            assert rows[repo][1:4] == short, (repo, rows[repo])
            assert rows[repo][5] == f"`{tag}`", (repo, rows[repo])
        for e in (e for e in entries if e["repo"] == repo):
            assert (e["sha"], e["core_sha"], e["pipos_sha"], e["tag"]) == (sha, core, pipos, tag)
    assert {e["repo"] for e in entries} == {"RETROS-BIOS", "MFBootAgent", "UOS", "Deitrix"}
    note = from_summary["Deitrix"][6]
    if case == "at-pins":
        assert note == ""
    if case in ("deitrix-behind", "pipos-newer-core"):
        assert f"Pip-OS: pinned `{pinned_pipos[:12]}`, built with `{built['Pip-OS'][0][:12]}`" in note
    if case == "pipos-newer-core":
        assert "library: pinned" in note and "image tag: pinned `tagold000000`, built with " \
            "`tagnew000000`" in note
    else:
        assert "library" not in note and "image tag" not in note
