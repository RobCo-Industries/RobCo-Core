"""boot.json, written from the harness, matches boot_args() and the spec. No QEMU needed."""

import json
import os
import re
import shutil
import subprocess
import sys

import pytest

from harness import bootjson
from harness.boot import BOARDS, boot_args
from harness.qemu import Qemu

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
SPEC = os.path.join(ROOT, "docs", "spec", "TESTING.md")

STUB = "/opt/pip os/fwstub.bin"
IMAGE = "/work/build/kernel.img"
SD = "/work/build/sd.img"

CASES = [(board, mode) for board in BOARDS for mode in ("auto", "svc", "hyp")]


def fill(args, **paths):
    out = []
    for a in args:
        for key, value in paths.items():
            a = a.replace("{" + key + "}", value)
        out.append(a)
    return out


def written():
    """The file as the command writes it, read back."""
    return json.loads(bootjson.dumps())


def spec_example():
    text = open(SPEC, encoding="utf-8").read()
    section = text.split("### boot.json", 1)[1]
    return json.loads(re.search(r"```json\n(.*?)```", section, re.S).group(1))


@pytest.mark.parametrize("name,mode", CASES)
@pytest.mark.parametrize("sd", [None, SD])
def test_filled_file_equals_boot_args(name, mode, sd):
    board = name
    doc = written()
    entry = doc["boards"].get(board)
    try:
        want = boot_args(IMAGE, board, mode, sd=sd, stub=STUB)
    except ValueError:
        # boot_args refuses the mode, so the file must not list it.
        assert mode not in entry["modes"]
        return
    chosen = entry["default_mode"] if mode == "auto" else mode
    got = fill(entry["modes"][chosen], stub=STUB, image=IMAGE)
    if sd is not None:
        got += fill(doc["sd"], sd=sd)
    assert got == want


def test_every_board_is_listed():
    assert sorted(written()["boards"]) == sorted(BOARDS)


def test_common_is_what_qemu_passes_after_machine():
    q = Qemu("qemu-system-arm", "raspi2b", qmp=False, null_serial=False)
    try:
        cmd = q.command_line()
    finally:
        shutil.rmtree(q.tmpdir, ignore_errors=True)
    assert cmd == ["qemu-system-arm", "-M", "raspi2b"] + written()["common"]


def test_written_file_equals_spec_example():
    assert written() == spec_example()


def test_only_known_placeholders():
    doc = written()
    strings = doc["sd"] + [a for b in doc["boards"].values() for m in b["modes"].values() for a in m]
    found = {p for s in strings for p in re.findall(r"\{[^}]*\}", s)}
    assert found == {"{stub}", "{image}", "{sd}"}


def test_command_prints_and_writes(tmp_path):
    tests_dir = os.path.join(ROOT, "tests")
    out = subprocess.run([sys.executable, "-m", "harness.bootjson"], cwd=tests_dir,
                         check=True, capture_output=True, text=True).stdout
    assert out == bootjson.dumps()
    path = tmp_path / "boot.json"
    subprocess.run([sys.executable, "-m", "harness.bootjson", "-o", str(path)],
                   cwd=tests_dir, check=True, capture_output=True)
    assert path.read_text(encoding="utf-8") == out


def test_written_file_is_valid():
    bootjson.validate(written())


def _broken(edit):
    doc = written()
    edit(doc)
    with pytest.raises(ValueError):
        bootjson.validate(doc)


@pytest.mark.parametrize("value", [0, 2, "1", 1.0, None])
def test_format_other_than_1_is_invalid(value):
    _broken(lambda d: d.update(format=value))


def test_default_mode_not_listed_is_invalid():
    _broken(lambda d: d["boards"]["raspi0"].update(default_mode="hyp"))


def test_unknown_placeholder_is_invalid():
    _broken(lambda d: d["boards"]["raspi2b"]["modes"]["hyp"].append("file={kernel}"))


def test_sd_placeholder_outside_sd_is_invalid():
    _broken(lambda d: d["boards"]["raspi3b"]["modes"]["svc"].append("file={sd}"))


def test_make_target_writes_the_file(tmp_path):
    path = tmp_path / "sub" / "boot.json"
    subprocess.run(["make", "-s", "-C", ROOT, "boot-json", f"BOOT_JSON={path}",
                    f"PYTHON={sys.executable}"], check=True, capture_output=True)
    assert path.read_text(encoding="utf-8") == bootjson.dumps()


def _make(*args, cwd=None):
    return subprocess.run(["make", "-s", "-C", os.path.abspath(ROOT), "boot-json", *args],
                          cwd=cwd, capture_output=True, text=True)


def test_make_target_with_relative_python(tmp_path):
    venv_py = os.path.join(ROOT, ".venv", "bin", "python")
    if not os.path.exists(venv_py):
        pytest.skip("no .venv")
    path = tmp_path / "boot.json"
    r = _make(f"BOOT_JSON={path}", "PYTHON=.venv/bin/python")
    assert r.returncode == 0, r.stderr
    assert path.read_text(encoding="utf-8") == bootjson.dumps()


def test_make_target_through_make_c_from_another_folder(tmp_path):
    path = tmp_path / "out" / "boot.json"
    r = _make(f"BOOT_JSON={path}", f"PYTHON={sys.executable}", cwd=tmp_path)
    assert r.returncode == 0, r.stderr
    assert path.read_text(encoding="utf-8") == bootjson.dumps()


def test_make_target_refuses_relative_output(tmp_path):
    r = _make("BOOT_JSON=rel/boot.json", f"PYTHON={sys.executable}", cwd=tmp_path)
    assert r.returncode != 0
    assert "must be an absolute path" in r.stderr
    assert not (tmp_path / "rel").exists()
    assert not os.path.exists(os.path.join(ROOT, "rel"))
