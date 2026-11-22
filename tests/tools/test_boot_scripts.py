"""Tests for tools/fetch_firmware.sh and tools/mkbootdir.sh.

Everything here runs offline except test_download, which needs
ROBCO_NET_TESTS=1. Tests that run curl skip when it is not on PATH.
"""

import os
import pathlib
import re
import shutil
import subprocess

import pytest

from test_rimg_tool import sealed

ROOT = pathlib.Path(__file__).resolve().parents[2]
FETCH = ROOT / "tools/fetch_firmware.sh"
MKBOOT = ROOT / "tools/mkbootdir.sh"
FIRMWARE = ("bootcode.bin", "start.elf", "fixup.dat")

needs_curl = pytest.mark.skipif(shutil.which("curl") is None,
                                reason="fetch_firmware.sh downloads with curl, which is not on PATH")
needs_sha256 = pytest.mark.skipif(shutil.which("sha256sum") is None and shutil.which("shasum") is None,
                                  reason="fetch_firmware.sh needs sha256sum or shasum")


def run(*args, env=None):
    return subprocess.run(["bash", *map(str, args)], capture_output=True, text=True,
                          env={**os.environ, **(env or {})})


def spec_config(kernel):
    """The terminal chain config.txt from BOARD_SUPPORT.md, with another kernel line."""
    text = (ROOT / "docs/spec/BOARD_SUPPORT.md").read_text()
    block = re.search(r"Terminal chain:\n\n```\n(.*?)```", text, re.S).group(1)
    return re.sub(r"^kernel=.*$", f"kernel={kernel}", block, flags=re.M)


@pytest.fixture
def image(tmp_path):
    """A minimal sealed rimg image."""
    path = tmp_path / "hello.img"
    path.write_bytes(sealed())
    return path


def test_fetch_usage():
    assert run(FETCH).returncode == 2
    assert run(FETCH, "--bogus", "x").returncode == 2
    assert run(FETCH, "a", "b").returncode == 2


def test_fetch_refuses_device():
    r = run(FETCH, "/dev/null")
    assert r.returncode == 1 and "refusing" in r.stderr


def test_fetch_refuses_symlink(tmp_path):
    outside = tmp_path / "outside"
    outside.write_text("keep")
    target = tmp_path / "boot"
    target.mkdir()
    (target / "start.elf").symlink_to(outside)
    r = run(FETCH, target)
    assert r.returncode == 1 and "refusing" in r.stderr
    assert outside.read_text() == "keep"


@needs_curl
@needs_sha256
def test_fetch_rejects_bad_hash(tmp_path):
    mirror = tmp_path / "mirror"
    mirror.mkdir()
    for f in FIRMWARE:
        (mirror / f).write_bytes(b"not firmware")
    target = tmp_path / "boot"
    r = run(FETCH, target, env={"FIRMWARE_URL": mirror.as_uri()})
    assert r.returncode == 1 and "nothing written" in r.stderr
    assert not any((target / f).exists() for f in FIRMWARE)


@needs_sha256
def test_fetch_check_fails_on_wrong_files(tmp_path):
    for f in FIRMWARE:
        (tmp_path / f).write_bytes(b"x")
    r = run(FETCH, "--check", tmp_path)
    assert r.returncode == 1 and "does not match" in r.stderr


def test_mkbootdir_usage(image, tmp_path):
    assert run(MKBOOT).returncode == 2
    assert run(MKBOOT, image).returncode == 2
    assert run(MKBOOT, image, tmp_path / "b", "--kernel-name").returncode == 2


def test_mkbootdir_config(image, tmp_path):
    target = tmp_path / "boot"
    r = run(MKBOOT, image, target, "--kernel-name", "RETROS.IMG", "--no-firmware")
    assert r.returncode == 0, r.stderr
    assert (target / "config.txt").read_text() == spec_config("RETROS.IMG")
    assert (target / "RETROS.IMG").read_bytes() == image.read_bytes()
    assert sorted(p.name for p in target.iterdir()) == ["RETROS.IMG", "config.txt"]


def test_mkbootdir_default_name(image, tmp_path):
    target = tmp_path / "boot"
    assert run(MKBOOT, image, target, "--no-firmware").returncode == 0
    assert (target / "config.txt").read_text().startswith("kernel=hello.img\n")


@pytest.mark.parametrize("name", ["a/b", "..", "start.elf", "CONFIG.TXT", "bad name"])
def test_mkbootdir_bad_name(image, tmp_path, name):
    r = run(MKBOOT, image, tmp_path / "boot", "--kernel-name", name, "--no-firmware")
    assert r.returncode == 1
    assert not (tmp_path / "boot").exists()


def test_mkbootdir_refuses_device(image):
    r = run(MKBOOT, image, "/dev/disk9", "--no-firmware")
    assert r.returncode == 1 and "never writes a device" in r.stderr


def test_mkbootdir_refuses_bad_image(tmp_path):
    bad = tmp_path / "bad.img"
    bad.write_bytes(b"\0" * 512)
    r = run(MKBOOT, bad, tmp_path / "boot", "--no-firmware")
    assert r.returncode == 1
    assert not (tmp_path / "boot").exists()


def test_mkbootdir_refuses_wrong_load_addr(tmp_path):
    """The firmware loads the kernel at 0x8000: an image linked elsewhere cannot boot."""
    other = tmp_path / "chain2.img"
    other.write_bytes(sealed(load_addr=0x00200000, subtype=4))
    r = run(MKBOOT, other, tmp_path / "boot", "--no-firmware")
    assert r.returncode == 1
    assert "0x200000" in r.stderr and "0x8000" in r.stderr
    assert not (tmp_path / "boot").exists()


def test_mkbootdir_refuses_missing_verifier(image, tmp_path):
    r = run(MKBOOT, image, tmp_path / "boot", "--no-firmware",
            env={"PYTHON": "robco-no-such-python"})
    assert r.returncode == 1 and "robco-no-such-python" in r.stderr
    assert not (tmp_path / "boot").exists()


@pytest.mark.skipif(os.environ.get("ROBCO_NET_TESTS") != "1",
                    reason="downloads firmware; set ROBCO_NET_TESTS=1")
@needs_curl
@needs_sha256
def test_download(image, tmp_path):
    target = tmp_path / "boot"
    r = run(MKBOOT, image, target, "--kernel-name", "RETROS.IMG")
    assert r.returncode == 0, r.stderr
    assert run(FETCH, "--check", target).returncode == 0
