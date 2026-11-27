"""Tests for tools/sfx_check.py on small made-up models."""

import json
import math
import os
import re
import subprocess
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TABLE = os.path.join(ROOT, "tools", "sfx_table.py")
CHECK = os.path.join(ROOT, "tools", "sfx_check.py")
sys.path.insert(0, os.path.join(ROOT, "tests"))
from harness.env import minimal_env  # noqa: E402

RATE = 20000


def tones_model(second_level=-6000):
    """Two partials with a glide, an attack and a release, split in two."""
    return {"format": 1, "rate_hz": RATE, "ceiling_steps": 90, "sounds": {"chime": {
        "len": 4000, "peak_steps": 60, "split": 1500, "layers": [
            {"kind": "tone", "hits": [[0, 0], [2000, -3000]], "len": 1800,
             "pitch_mhz": [880000, 660000], "level_mdb": 0,
             "partials": [[10000, 0], [30000, second_level]],
             "attack_us": 5000, "release_us": 20000,
             "env": [[0, 0], [90000, -12000]]}]}}}


def noise_model():
    """A burst of band-passed noise with a soft tail."""
    return {"format": 1, "rate_hz": RATE, "ceiling_steps": 90, "sounds": {"burst": {
        "len": 8000, "peak_steps": 70, "layers": [
            {"kind": "noise", "seed": 5, "level_mdb": 0, "hits": [[0, 0], [4000, -4000]],
             "env": [[0, -40000], [3000, 0], [150000, -24000], [200000, -40000]],
             "bands": [[300, -20000], [1500, 0], [5000, -4000], [9000, -30000]]}]}}}


def loop_model():
    """A one second noise loop that folds over itself, with a steady hum."""
    n = RATE  # samples in one second
    fold = 400
    return {"format": 1, "rate_hz": RATE, "ceiling_steps": 90, "sounds": {"hum": {
        "len": n, "peak_steps": 30, "loop": True, "layers": [
            {"kind": "noise", "seed": 9, "level_mdb": -6000, "hits": [[0, 0]],
             "fold": fold, "env": [[0, 0], [(n + fold) * 1000000 // RATE, 0]],
             "bands": [[200, -10000], [1000, 0], [4000, -6000], [8000, -30000]]},
            {"kind": "tone", "hits": [[0, 0]], "len": n,
             "pitch_mhz": [120000, 120000], "level_mdb": -3000,
             "partials": [[10000, 0], [20000, -8000]]}]}}}


MODELS = {"tones": tones_model, "noise": noise_model, "loop": loop_model}


def run(tool, *args):
    return subprocess.run([sys.executable, "-I", tool, *map(str, args)],
                          capture_output=True, text=True, env=minimal_env())


def dump(tmp_path, model, name="model.json", folder="out"):
    p = tmp_path / name
    p.write_text(json.dumps(model))
    d = tmp_path / folder
    r = run(TABLE, p, "--dump", d)
    assert r.returncode == 0, r.stderr
    return p, d


def only_table(d):
    files = sorted(os.listdir(d))
    return [os.path.join(d, f) for f in files]


@pytest.mark.parametrize("kind", sorted(MODELS))
def test_dumped_tables_pass(tmp_path, kind):
    p, d = dump(tmp_path, MODELS[kind]())
    r = run(CHECK, p, d)
    assert r.returncode == 0, r.stdout + r.stderr
    assert re.search(r" %s band mean \d+\.\d\d .*: ok$" % kind, r.stdout.strip()), r.stdout


def test_partial_6db_off_fails(tmp_path):
    p, _ = dump(tmp_path, tones_model())
    _, d2 = dump(tmp_path, tones_model(second_level=0), name="other.json", folder="other")
    r = run(CHECK, p, d2)
    assert r.returncode == 1, r.stdout + r.stderr
    assert "FAIL" in r.stdout and "band" in r.stdout


def _rewrite(path, fn):
    with open(path, "rb") as f:
        data = bytearray(f.read())
    fn(data)
    with open(path, "wb") as f:
        f.write(bytes(data))


def test_frame_8db_off_fails(tmp_path):
    p, d = dump(tmp_path, noise_model())
    scale = 10 ** (-8 / 20)

    def quiet(data):
        loud = max(range(0, len(data) - 100, 100),
                   key=lambda i: sum((b - 128) ** 2 for b in data[i:i + 100]))
        start = loud + 400  # a stretch past the loudest one keeps the peak
        for i in range(start, start + 100):
            data[i] = 128 + int(round((data[i] - 128) * scale))
    _rewrite(os.path.join(d, "burst.u8"), quiet)
    r = run(CHECK, p, d)
    assert r.returncode == 1, r.stdout + r.stderr
    assert "frame" in r.stdout


def test_wrong_peak_fails(tmp_path):
    p, d = dump(tmp_path, tones_model())

    def move(data):
        i = max(range(len(data)), key=lambda k: abs(data[k] - 128))
        data[i] += -1 if data[i] > 128 else 1
    # The loudest sample may lie in either part.
    for f in only_table(d):
        _rewrite(f, move)
    r = run(CHECK, p, d)
    assert r.returncode == 1, r.stdout + r.stderr
    assert "peak is" in r.stdout


def test_missing_table_gives_2(tmp_path):
    p, d = dump(tmp_path, tones_model())
    os.remove(os.path.join(d, "chime_2.u8"))
    r = run(CHECK, p, d)
    assert r.returncode == 2, r.stdout + r.stderr


def test_wrong_length_gives_2(tmp_path):
    p, d = dump(tmp_path, noise_model())
    _rewrite(os.path.join(d, "burst.u8"), lambda data: data.append(128))
    r = run(CHECK, p, d)
    assert r.returncode == 2, r.stdout + r.stderr


def test_bad_model_gives_2(tmp_path):
    p = tmp_path / "model.json"
    p.write_text('{"format": 1.0}')
    r = run(CHECK, p, tmp_path)
    assert r.returncode == 2, r.stdout + r.stderr


@pytest.mark.parametrize("args", [[], ["a"], ["a", "b", "c"], ["--x", "b"]])
def test_bad_command_line_gives_64(args):
    r = run(CHECK, *args)
    assert r.returncode == 64, r.stdout + r.stderr


def test_does_not_import_the_generator():
    with open(CHECK, encoding="utf-8") as f:
        src = f.read()
    assert not re.search(r"^\s*(import|from)\s+sfx_table\b", src, re.M)
    assert "import_module" not in src and "__import__" not in src
    assert "sfx_table" not in re.sub(r'""".*?"""|#.*', "", src, flags=re.S)


def test_one_line_per_sound(tmp_path):
    m = tones_model()
    m["sounds"]["other"] = noise_model()["sounds"]["burst"]
    p, d = dump(tmp_path, m)
    r = run(CHECK, p, d)
    assert r.returncode == 0, r.stdout + r.stderr
    assert len(r.stdout.strip().splitlines()) == 2
    assert math.isfinite(float(re.findall(r"worst (\d+\.\d\d)", r.stdout)[0]))
