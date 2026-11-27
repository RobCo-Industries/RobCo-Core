"""tools/release/assemble.py on stand-in card folders."""
import gzip
import hashlib
import json
import os
import subprocess
import sys

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
TOOL = os.path.join(ROOT, "tools", "release", "assemble.py")
REPOS = ("RobCo-Core", "RETROS-BIOS", "MFBootAgent", "UOS", "Pip-OS", "Deitrix")
VERSIONS = "| Repository | x |\n"


def tool(*args):
    return subprocess.run([sys.executable, "-I", TOOL, *args],
                          capture_output=True, text=True)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def card(folder, files):
    folder.mkdir()
    for name, data in files.items():
        (folder / name).write_bytes(data)
    (folder / "VERSIONS.txt").write_text(VERSIONS)
    (folder / "SHA256SUMS").write_text("".join(f"{sha(d)}  {n}\n" for n, d in files.items()))
    return folder


@pytest.fixture
def inputs(tmp_path):
    manifest = tmp_path / "manifest.json"
    manifest.write_text(json.dumps({"suite": "suite-2026.12", "repositories": {
        name: {"version": "0.1.0", "commit": f"{i}" * 40} for i, name in enumerate(REPOS)}}))
    term = card(tmp_path / "t", {"robco-terminal.img": b"T" * 5000})
    pip = card(tmp_path / "p", {"robco-pipboy.img": b"P" * 7000, "pipos.img": b"K"})
    notices = tmp_path / "NOTICES.txt"
    notices.write_text("MIT\n")
    fetch = tmp_path / "fetch_firmware.sh"
    fetch.write_text("#!/bin/sh\n")
    out = tmp_path / "dist"
    args = ["--manifest", str(manifest), "--terminal", str(term), "--pipboy", str(pip),
            "--notices", str(notices), "--fetch-firmware", str(fetch), "--out", str(out)]
    return args, term, pip, out


def test_builds_dist(inputs):
    args, term, pip, out = inputs
    r = tool(*args)
    assert r.returncode == 0, r.stdout
    assets = out / "assets"
    assert sorted(os.listdir(assets)) == sorted([
        "robco-terminal.img.gz", "robco-pipboy.img.gz", "fetch_firmware.sh",
        "NOTICES.txt", "VERSIONS.txt", "suite-2026.12.json", "SHA256SUMS"])
    assert gzip.decompress((assets / "robco-terminal.img.gz").read_bytes()) == b"T" * 5000
    assert gzip.decompress((assets / "robco-pipboy.img.gz").read_bytes()) == b"P" * 7000
    sums = (assets / "SHA256SUMS").read_text().splitlines()
    assert len(sums) == 6
    for line in sums:
        digest, name = line.split("  ")
        assert sha((assets / name).read_bytes()) == digest
    notes = (out / "NOTES.md").read_text()
    assert "# suite-2026.12" in notes
    assert sha(b"T" * 5000) in notes and "`" + "5" * 40 + "`" in notes


def test_gzip_has_no_name_and_no_time(inputs):
    args, _, _, out = inputs
    assert tool(*args).returncode == 0
    head = (out / "assets" / "robco-terminal.img.gz").read_bytes()[:10]
    assert head[3] & 0x08 == 0, "a file name is stored"
    assert head[4:8] == b"\0\0\0\0", "a time is stored"


def test_same_inputs_same_bytes(inputs, tmp_path):
    args, _, _, out = inputs
    assert tool(*args).returncode == 0
    first = (out / "assets" / "SHA256SUMS").read_bytes()
    args[-1] = str(tmp_path / "dist2")
    assert tool(*args).returncode == 0
    assert (tmp_path / "dist2" / "assets" / "SHA256SUMS").read_bytes() == first


def test_changed_card_fails(inputs):
    args, term, _, out = inputs
    (term / "robco-terminal.img").write_bytes(b"X")
    r = tool(*args)
    assert r.returncode == 1
    assert "::error::assemble:" in r.stdout and "robco-terminal.img has SHA-256" in r.stdout
    assert not out.exists()


def test_missing_card_fails(inputs):
    args, _, pip, _ = inputs
    (pip / "SHA256SUMS").write_text(f"{sha(b'K')}  pipos.img\n")
    r = tool(*args)
    assert r.returncode == 1 and "has no robco-pipboy.img" in r.stdout


def test_different_versions_fail(inputs):
    args, _, pip, _ = inputs
    (pip / "VERSIONS.txt").write_text("other\n")
    r = tool(*args)
    assert r.returncode == 1 and "different VERSIONS.txt" in r.stdout


def test_bad_sums_line_fails(inputs):
    args, term, _, _ = inputs
    (term / "SHA256SUMS").write_text("nonsense\n")
    r = tool(*args)
    assert r.returncode == 1 and "bad line" in r.stdout


def test_existing_out_fails(inputs):
    args, _, _, out = inputs
    out.mkdir()
    r = tool(*args)
    assert r.returncode == 1 and "exists already" in r.stdout


def test_bad_manifest_fails(inputs, tmp_path):
    args, _, _, _ = inputs
    bad = tmp_path / "bad.json"
    bad.write_text(json.dumps({"suite": "other-1.0.0", "repositories": {}}))
    args[1] = str(bad)
    r = tool(*args)
    assert r.returncode == 1 and "not a suite manifest" in r.stdout


def test_bad_suite_fails(inputs, tmp_path):
    args, _, _, _ = inputs
    data = json.loads(open(args[1]).read())
    data["suite"] = "v1"
    bad = tmp_path / "bad.json"
    bad.write_text(json.dumps(data))
    args[1] = str(bad)
    r = tool(*args)
    assert r.returncode == 1 and "is not suite-YYYY.MM" in r.stdout
