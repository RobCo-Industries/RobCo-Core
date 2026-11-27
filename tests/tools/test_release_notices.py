"""tools/release/notices.py on a small fake work folder."""
import os
import subprocess
import sys

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
TOOL = os.path.join(ROOT, "tools", "release", "notices.py")
REPOS = ("RobCo-Core", "RETROS-BIOS", "MFBootAgent", "UOS", "Pip-OS", "Deitrix")


def tool(*args):
    return subprocess.run([sys.executable, "-I", TOOL, *args],
                          capture_output=True, text=True)


@pytest.fixture
def work(tmp_path):
    w = tmp_path / "w"
    for repo in REPOS:
        (w / repo).mkdir(parents=True)
        (w / repo / "LICENSE").write_text(f"MIT text of {repo}.\r\n\r\n")
    third = w / "RobCo-Core" / "third_party"
    for rel, text in (("zfont/LICENSE", "Z terms.\n"), ("afs/LICENSE.txt", "A terms.\n"),
                      ("afs/ff.c", "code\n"), ("afs/PROVENANCE", "skipped terms\n"),
                      ("pfont/PROVENANCE", "P terms.\n"), ("pfont/README.md", "no licence\n")):
        (third / rel).parent.mkdir(parents=True, exist_ok=True)
        (third / rel).write_text(text)
    return w


def test_joins_in_fixed_order(work, tmp_path):
    out = tmp_path / "NOTICES.txt"
    r = tool("--work", str(work), "--out", str(out))
    assert r.returncode == 0, r.stdout
    text = out.read_text()
    heads = [l for l in text.splitlines() if l.startswith("==== ")]
    assert heads == [f"==== {r}: LICENSE ====" for r in REPOS] + [
        "==== RobCo-Core: third_party/afs/LICENSE.txt ====",
        "==== RobCo-Core: third_party/pfont/PROVENANCE ====",
        "==== RobCo-Core: third_party/zfont/LICENSE ===="]
    assert "MIT text of Deitrix.\n" in text and "\r" not in text
    assert "P terms." in text and "skipped terms" not in text
    assert "code" not in text and "no licence" not in text


def test_folder_with_neither_fails(work, tmp_path):
    (work / "RobCo-Core" / "third_party" / "pfont" / "PROVENANCE").unlink()
    out = tmp_path / "NOTICES.txt"
    r = tool("--work", str(work), "--out", str(out))
    assert r.returncode == 1
    assert "third_party/pfont" in r.stdout
    assert not out.exists()


def test_same_bytes_each_run(work, tmp_path):
    a, b = tmp_path / "a.txt", tmp_path / "b.txt"
    assert tool("--work", str(work), "--out", str(a)).returncode == 0
    assert tool("--work", str(work), "--out", str(b)).returncode == 0
    assert a.read_bytes() == b.read_bytes()


@pytest.mark.parametrize("missing", ["UOS/LICENSE", "RobCo-Core/LICENSE"])
def test_missing_licence_fails(work, tmp_path, missing):
    (work / missing).unlink()
    out = tmp_path / "NOTICES.txt"
    r = tool("--work", str(work), "--out", str(out))
    assert r.returncode == 1
    assert r.stdout.startswith("::error::notices:")
    assert not out.exists()


def test_missing_third_party_fails(work, tmp_path):
    import shutil
    shutil.rmtree(work / "RobCo-Core" / "third_party")
    out = tmp_path / "NOTICES.txt"
    assert tool("--work", str(work), "--out", str(out)).returncode == 1
    assert tool("--work", str(tmp_path / "none"), "--out", str(out)).returncode == 1
    assert not out.exists()
