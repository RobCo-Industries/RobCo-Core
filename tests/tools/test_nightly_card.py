"""tools/nightly/pack_card.py: the firmware check and the packed files."""
import hashlib
import os
import shutil
import subprocess
import sys

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
PACK = os.path.join(ROOT, "tools", "nightly", "pack_card.py")
sys.path.insert(0, os.path.join(ROOT, "tools", "nightly"))
import pack_card  # noqa: E402


def test_firmware_names_in_any_folder_and_case():
    files = ["::/RETROS.IMG", "::/ROBCO/", "::/BOOTCODE.BIN", "::/boot/start.elf",
             "::/fixup.dat.txt", "::/FIXUP.DAT"]
    assert pack_card.firmware_on(files) == ["::/BOOTCODE.BIN", "::/boot/start.elf", "::/FIXUP.DAT"]


needs_mtools = pytest.mark.skipif(not shutil.which("mformat"), reason="mtools is not installed")


def make_card(tmp_path, names):
    stage = tmp_path / "stage"
    for name in names:
        (stage / name).parent.mkdir(parents=True, exist_ok=True)
        (stage / name).write_bytes(name.encode())
    card = tmp_path / "sd.img"
    subprocess.run([os.path.join(ROOT, "tools", "mksdimage.sh"), str(card), "64", str(stage)],
                   check=True, capture_output=True)
    return card


def pack(tmp_path, card, *extra):
    versions = tmp_path / "VERSIONS.txt"
    versions.write_text("| table |\n")
    return subprocess.run([sys.executable, "-I", PACK, "--card", str(card), "--name",
                           "robco-terminal.img", "--versions", str(versions),
                           "--out", str(tmp_path / "out"), *extra],
                          capture_output=True, text=True)


@needs_mtools
def test_a_clean_card_is_packed(tmp_path):
    card = make_card(tmp_path, ["RETROS.IMG", "ROBCO/UOS.SYS"])
    kernel = tmp_path / "pipos.img"
    kernel.write_bytes(b"kernel")
    proc = pack(tmp_path, card, "--extra", str(kernel))
    assert proc.returncode == 0, proc.stdout + proc.stderr
    out = tmp_path / "out"
    assert sorted(os.listdir(out)) == ["SHA256SUMS", "VERSIONS.txt", "pipos.img",
                                       "robco-terminal.img"]
    sums = (out / "SHA256SUMS").read_text().splitlines()
    assert sums == [f"{hashlib.sha256(card.read_bytes()).hexdigest()}  robco-terminal.img",
                    f"{hashlib.sha256(b'kernel').hexdigest()}  pipos.img"]


@needs_mtools
def test_firmware_on_the_card_fails(tmp_path):
    card = make_card(tmp_path, ["RETROS.IMG", "start.elf"])
    proc = pack(tmp_path, card)
    assert proc.returncode == 1
    assert "Pi firmware on" in proc.stdout and "start.elf" in proc.stdout
    assert not (tmp_path / "out").exists()


def test_an_unreadable_card_fails(tmp_path):
    card = tmp_path / "sd.img"
    card.write_bytes(b"\0" * 4096)
    proc = pack(tmp_path, card)
    assert proc.returncode == 1
    assert "::error::pack_card:" in proc.stdout
