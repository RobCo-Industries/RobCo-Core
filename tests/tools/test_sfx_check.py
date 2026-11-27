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


def quiet_loop_model():
    """A quiet noise loop whose bands fall about 28 dB across the kept range."""
    n = RATE // 2  # samples in half a second
    fold = 400
    return {"format": 1, "rate_hz": RATE, "ceiling_steps": 90, "sounds": {"quiet": {
        "len": n, "peak_steps": 10, "loop": True, "layers": [
            {"kind": "noise", "seed": 3, "level_mdb": 0, "hits": [[0, 0]],
             "fold": fold, "env": [[0, 0], [(n + fold) * 1000000 // RATE, 0]],
             "bands": [[0, 0], [1000, 0], [9000, -28000], [10000, -28000]]}]}}}


def _worst_band(line):
    return float(re.search(r"band mean [\d.]+ median [\d.]+ worst ([\d.]+) dB", line).group(1))


def test_quiet_loop_needs_the_rounding_floor(tmp_path, monkeypatch):
    p, d = dump(tmp_path, quiet_loop_model())
    r = run(CHECK, p, d)
    assert r.returncode == 0, r.stdout + r.stderr
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    try:
        import sfx_check
    finally:
        sys.path.pop(0)
    model = sfx_check.load_model(str(p))
    with_floor, ok = sfx_check.check_sound(model, "quiet", str(d))
    assert ok, with_floor
    monkeypatch.setattr(sfx_check, "ROUND_POWER", 0.0)
    without, _ = sfx_check.check_sound(model, "quiet", str(d))
    assert _worst_band(without) > _worst_band(with_floor), (with_floor, without)


def test_noise_above_15_khz_passes_with_a_note(tmp_path):
    rate = 48000
    m = {"format": 1, "rate_hz": rate, "ceiling_steps": 90, "sounds": {"hiss": {
        "len": 9600, "peak_steps": 60, "layers": [
            {"kind": "noise", "seed": 4, "level_mdb": 0, "hits": [[0, 0]],
             "env": [[0, -60000], [2000, 0], [198000, 0], [200000, -60000]],
             "bands": [[16000, 0], [23000, 0]]}]}}}
    p, d = dump(tmp_path, m)
    r = run(CHECK, p, d)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "bands not judged, no power under 15 kHz: ok" in r.stdout, r.stdout


def tone_model(peak):
    return {"format": 1, "rate_hz": RATE, "ceiling_steps": 90, "sounds": {"beep": {
        "len": 4000, "peak_steps": peak, "layers": [
            {"kind": "tone", "hits": [[0, 0]], "len": 4000,
             "pitch_mhz": [440000, 440000], "level_mdb": 0,
             "partials": [[10000, 0]]}]}}}


@pytest.mark.parametrize("peak", [1, 2, 4])
def test_quiet_tone_passes(tmp_path, peak):
    p, d = dump(tmp_path, tone_model(peak))
    r = run(CHECK, p, d)
    assert r.returncode == 0, r.stdout + r.stderr


def short_model(peak=50):
    """A 15 ms burst of noise: three frames."""
    return {"format": 1, "rate_hz": RATE, "ceiling_steps": 90, "sounds": {"tick": {
        "len": 300, "peak_steps": peak, "layers": [
            {"kind": "noise", "seed": 7, "level_mdb": 0, "hits": [[0, 0]],
             "env": [[0, -60000], [1000, 0], [14000, 0], [15000, -60000]],
             "bands": [[500, 0], [6000, 0]]}]}}}


def test_short_noise_passes_with_a_note(tmp_path):
    p, d = dump(tmp_path, short_model())
    r = run(CHECK, p, d)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "bands not judged, under 20 ms: ok" in r.stdout, r.stdout


def test_short_noise_frame_8db_off_fails(tmp_path):
    p, d = dump(tmp_path, short_model())
    scale = 10 ** (-8 / 20)

    def quiet(data):
        loud = max(range(len(data)), key=lambda k: abs(data[k] - 128))
        start = 0 if loud >= 100 else 100  # one 5 ms frame without the peak
        for i in range(start, start + 100):
            data[i] = 128 + int(round((data[i] - 128) * scale))
    _rewrite(os.path.join(d, "tick.u8"), quiet)
    r = run(CHECK, p, d)
    assert r.returncode == 1, r.stdout + r.stderr
    assert "frame" in r.stdout


def test_short_noise_wrong_peak_fails(tmp_path):
    _, d = dump(tmp_path, short_model())
    p2 = tmp_path / "other.json"
    p2.write_text(json.dumps(short_model(peak=51)))
    r = run(CHECK, p2, d)
    assert r.returncode == 1, r.stdout + r.stderr
    assert "peak is" in r.stdout


def _bad(fn):
    m = noise_model()
    fn(m["sounds"]["burst"])
    return m


BAD_MODELS = {
    "same band frequency": lambda s: s["layers"][0].update(
        bands=[[300, 0], [300, -3000], [9000, -30000]]),
    "falling band frequency": lambda s: s["layers"][0].update(
        bands=[[3000, 0], [300, -3000]]),
    "fold past half": lambda s: s["layers"][0].update(fold=3000),
    "env of no sample": lambda s: s["layers"][0].update(env=[[0, 0], [0, 0]]),
    "hit past the end": lambda s: s["layers"][0].update(hits=[[8000, 0]]),
    "env going back": lambda s: s["layers"][0].update(env=[[0, 0], [5000, 0], [4000, 0]]),
    "no layer": lambda s: s.update(layers=[]),
    "no hit": lambda s: s["layers"][0].update(hits=[]),
    "zero len": lambda s: s.update(len=0),
    "zero pitch": lambda s: s["layers"].append(
        {"kind": "tone", "hits": [[0, 0]], "len": 100, "pitch_mhz": [0, 440000],
         "level_mdb": 0, "partials": [[10000, 0]]}),
    "zero tone len": lambda s: s["layers"].append(
        {"kind": "tone", "hits": [[0, 0]], "len": 0, "pitch_mhz": [440000, 440000],
         "level_mdb": 0, "partials": [[10000, 0]]}),
}


@pytest.mark.parametrize("case", sorted(BAD_MODELS))
def test_invalid_model_gives_2(tmp_path, case):
    p = tmp_path / "model.json"
    p.write_text(json.dumps(_bad(BAD_MODELS[case])))
    r = run(CHECK, p, tmp_path)
    assert r.returncode == 2, r.stdout + r.stderr
    assert "burst" in r.stderr, r.stderr
