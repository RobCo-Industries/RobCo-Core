"""audio.c refuses out-of-range audio macros at build time."""

import pathlib
import shutil
import subprocess

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]
CC = "arm-none-eabi-gcc"

pytestmark = pytest.mark.skipif(shutil.which(CC) is None, reason="needs arm-none-eabi-gcc")


def compile_audio(*defines):
    cmd = [CC, "-mcpu=arm1176jzf-s", "-marm", "-std=gnu11", "-ffreestanding", "-Wall",
           "-Wextra", "-Werror", "-fsyntax-only", "-I", str(ROOT / "include"),
           *[f"-D{d}" for d in defines], str(ROOT / "src/drv/audio.c")]
    return subprocess.run(cmd, capture_output=True, text=True)


def test_defaults_build():
    r = compile_audio()
    assert r.returncode == 0, r.stderr


@pytest.mark.parametrize("define", ["ROBCO_AUDIO_BEEP_HZ=20", "ROBCO_AUDIO_BEEP_HZ=20000",
                                    "ROBCO_AUDIO_PIN0=53 -DROBCO_AUDIO_FN0=7"])
def test_edges_build(define):
    r = compile_audio(*define.split(" -D"))
    assert r.returncode == 0, r.stderr


@pytest.mark.parametrize("defines,macro", [
    (["ROBCO_AUDIO_BEEP_HZ=19"], "ROBCO_AUDIO_BEEP_HZ"),
    (["ROBCO_AUDIO_BEEP_HZ=20001"], "ROBCO_AUDIO_BEEP_HZ"),
    (["ROBCO_AUDIO_BEEP_HZ=0"], "ROBCO_AUDIO_BEEP_HZ"),
    (["ROBCO_AUDIO_BEEP_MS=0"], "ROBCO_AUDIO_BEEP_MS"),
    (["ROBCO_AUDIO_VOLUME=101"], "ROBCO_AUDIO_VOLUME"),
    (["ROBCO_AUDIO_PIN0=54", "ROBCO_AUDIO_FN0=4"], "ROBCO_AUDIO_PIN0"),
    (["ROBCO_AUDIO_PIN0=18", "ROBCO_AUDIO_FN0=8"], "ROBCO_AUDIO_PIN0"),
    (["ROBCO_AUDIO_PIN0=18", "ROBCO_AUDIO_FN0=2", "ROBCO_AUDIO_PIN1=60", "ROBCO_AUDIO_FN1=2"],
     "ROBCO_AUDIO_PIN1"),
])
def test_bad_values_stop_the_build(defines, macro):
    r = compile_audio(*defines)
    assert r.returncode != 0
    assert macro in r.stderr
