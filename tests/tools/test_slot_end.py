"""The linker script refuses an image that does not fit below its slot end."""

import pathlib
import shutil
import subprocess

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]

pytestmark = pytest.mark.skipif(shutil.which("arm-none-eabi-gcc") is None,
                                reason="needs arm-none-eabi-gcc")


def build_hello(tmp_path, *args):
    return subprocess.run(["make", "-C", str(ROOT / "examples/hello"), "CI=1",
                           f"BUILD={tmp_path}/build", *args],
                          capture_output=True, text=True)


def test_slot_end_too_low_fails(tmp_path):
    r = build_hello(tmp_path, "SLOT_END=0x10000")
    assert r.returncode != 0
    assert "do not fit below ROBCO_SLOT_END" in r.stdout + r.stderr


def test_slot_end_from_config_links(tmp_path):
    r = build_hello(tmp_path)
    assert r.returncode == 0, r.stdout + r.stderr
    ld = (tmp_path / "build/image.ld").read_text()
    assert "__slot_end = 0x00100000" in ld
