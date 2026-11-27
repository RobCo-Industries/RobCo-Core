"""Tests for tools/sfx_check.py on small made-up models."""

import json
import math
import os
import re
import subprocess
import sys
import time

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
    assert re.search(r" %s bands \d+ Hz, frames \d+\.\d ms, band mean \d+\.\d\d .*: ok$" % kind,
                     r.stdout.strip()), r.stdout


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


def test_short_noise_passes(tmp_path):
    p, d = dump(tmp_path, short_model())
    r = run(CHECK, p, d)
    assert r.returncode == 0, r.stdout + r.stderr
    # Its bands hold too few independent values and are judged by chance.
    assert re.search(r"bands \d+ Hz, frames 5\.0 ms, .*, thin \d+ bands, 0 frames, .*: ok$",
                     r.stdout.strip()), r.stdout


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


def _import_check():
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    try:
        import sfx_check
    finally:
        sys.path.pop(0)
    return sfx_check


def test_high_sine_in_place_of_a_tone_fails(tmp_path):
    rate = 48000
    m = {"format": 1, "rate_hz": rate, "ceiling_steps": 90, "sounds": {"beep": {
        "len": 4800, "peak_steps": 60, "layers": [
            {"kind": "tone", "hits": [[0, 0]], "len": 4800,
             "pitch_mhz": [1000000, 1000000], "level_mdb": 0,
             "partials": [[10000, 0]], "attack_us": 2000, "release_us": 2000}]}}}
    p, d = dump(tmp_path, m)
    _rewrite(os.path.join(d, "beep.u8"), lambda data: data.__setitem__(
        slice(None), bytes(128 + round(60 * math.sin(2 * math.pi * 18000 * i / rate))
                           for i in range(len(data)))))
    r = run(CHECK, p, d)
    assert r.returncode == 1, r.stdout + r.stderr
    assert re.search(r"power under 15 kHz is \d+\.\d % of the table, the model predicts"
                     r" \d+\.\d %", r.stdout), r.stdout


def test_loop_with_coincident_repeated_hits_passes(tmp_path):
    m = loop_model()
    noise = next(iter(m["sounds"].values()))["layers"][0]
    noise.update(repeat=True, level_mdb=-18000, hits=[[0, 0], [0, -2000], [0, 0], [0, -6000]])
    p, d = dump(tmp_path, m)
    r = run(CHECK, p, d)
    assert r.returncode == 0, r.stdout + r.stderr


def tick_model():
    """A 5 ms noise tick: one frame."""
    return {"format": 1, "rate_hz": RATE, "ceiling_steps": 90, "sounds": {"tick": {
        "len": 100, "peak_steps": 40, "layers": [
            {"kind": "noise", "seed": 2, "level_mdb": 0, "hits": [[0, 0]],
             "env": [[0, -60000], [500, 0], [4500, 0], [5000, -60000]],
             "bands": [[500, 0], [6000, 0]]}]}}}


def test_one_frame_tick_checks_its_level(tmp_path):
    p, d = dump(tmp_path, tick_model())
    r = run(CHECK, p, d)
    assert r.returncode == 0, r.stdout + r.stderr
    assert re.search(r"worst \d+\.\d\d dB, thin \d+ bands, \d+ frames, "
                     r"level -?\d+\.\d\d dB: ok", r.stdout), r.stdout

    def spike(data):
        data[:] = bytes([128] * len(data))
        data[50] = 128 + 40
    _rewrite(os.path.join(d, "tick.u8"), spike)
    r = run(CHECK, p, d)
    assert r.returncode == 1, r.stdout + r.stderr
    assert re.search(r"level \d+\.\d\d dB outside -6\.00 to 14\.00", r.stdout), r.stdout


def test_tones_print_no_level(tmp_path):
    p, d = dump(tmp_path, tone_model(40))
    r = run(CHECK, p, d)
    assert r.returncode == 0 and "level" not in r.stdout, r.stdout


@pytest.mark.parametrize("change,field", [
    (lambda s: s["layers"][0].update(hits=[[0, 10000000]]), "hits"),
    (lambda s: s["layers"][0].update(env=[[0, 0], [10 ** 12, 0]]), "env"),
])
def test_huge_values_give_2(tmp_path, change, field):
    p = tmp_path / "model.json"
    p.write_text(json.dumps(_bad(change)))
    r = run(CHECK, p, tmp_path)
    assert r.returncode == 2, r.stdout + r.stderr
    assert "burst" in r.stderr and field in r.stderr, r.stderr


SWEEP_RATE = 8000
MAX_LEN = 262144
MAX_US = MAX_LEN * 1000000 // 8000
MDB = (-120000, 20000)


def sweep_model():
    """A small sound with one tone and one noise layer."""
    return {"format": 1, "rate_hz": SWEEP_RATE, "ceiling_steps": 90, "sounds": {"s": {
        "len": 400, "peak_steps": 40, "layers": [
            {"kind": "tone", "hits": [[0, 0]], "len": 200, "pitch_mhz": [440000, 440000],
             "level_mdb": 0, "partials": [[10000, 0]], "attack_us": 1000,
             "release_us": 1000, "env": [[0, 0], [10000, -6000]]},
            {"kind": "noise", "seed": 1, "level_mdb": 0, "hits": [[0, 0]], "fold": 10,
             "env": [[0, 0], [25000, 0]], "bands": [[100, 0], [3000, 0]]}]}}}


def _set(path, value):
    def fn(m):
        obj = m
        for k in path[:-1]:
            obj = obj[k]
        obj[path[-1]] = value
    return fn


def _sweep_cases():
    s = ("sounds", "s")
    tone, noise = s + ("layers", 0), s + ("layers", 1)
    cases = {}
    for name, path, lo, hi in [
            ("rate_hz", ("rate_hz",), 8000, 48000),
            ("ceiling_steps", ("ceiling_steps",), 40, 127),
            ("len", s + ("len",), 1, MAX_LEN),
            ("peak_steps", s + ("peak_steps",), 1, 90),
            ("split", s + ("split",), 1, 399),
            ("tone level_mdb", tone + ("level_mdb",), *MDB),
            ("tone len", tone + ("len",), 1, MAX_LEN),
            ("pitch_mhz from", tone + ("pitch_mhz", 0), 1, SWEEP_RATE * 500),
            ("pitch_mhz to", tone + ("pitch_mhz", 1), 1, SWEEP_RATE * 500),
            ("partial multiple", tone + ("partials", 0, 0), 1, 1000000),
            ("partial level", tone + ("partials", 0, 1), *MDB),
            ("attack_us", tone + ("attack_us",), 0, 25000),
            ("release_us", tone + ("release_us",), 0, 25000),
            ("tone env time", tone + ("env", 1, 0), 0, MAX_US),
            ("tone env level", tone + ("env", 1, 1), *MDB),
            ("tone hit start", tone + ("hits", 0, 0), 0, 399),
            ("tone hit level", tone + ("hits", 0, 1), *MDB),
            ("noise hit start", noise + ("hits", 0, 0), 0, 399),
            ("noise hit level", noise + ("hits", 0, 1), *MDB),
            ("noise level_mdb", noise + ("level_mdb",), *MDB),
            ("noise env time", noise + ("env", 1, 0), 125, MAX_US),
            ("noise env level", noise + ("env", 0, 1), *MDB),
            ("band frequency low", noise + ("bands", 0, 0), 0, 2999),
            ("band frequency high", noise + ("bands", 1, 0), 101, 4000),
            ("band level", noise + ("bands", 0, 1), *MDB),
            ("fold", noise + ("fold",), 0, 100)]:
        cases[name + " low"] = _set(path, lo)
        cases[name + " high"] = _set(path, hi)
    return cases


SWEEP = _sweep_cases()


@pytest.mark.parametrize("case", sorted(SWEEP))
def test_boundary_values_never_crash(tmp_path, case):
    """Each bound the generator's validator accepts, one at a time. The
    tool may pass, fail or refuse, but never raise."""
    sfx_check = _import_check()
    m = sweep_model()
    SWEEP[case](m)
    s = m["sounds"]["s"]
    s["peak_steps"] = min(s["peak_steps"], m["ceiling_steps"])
    # Keep the bounds that depend on another field inside the validator's.
    tone, noise = s["layers"]
    for key in ("attack_us", "release_us"):
        tone[key] = min(tone[key], tone["len"] * 1000000 // m["rate_hz"])
    span = -(-noise["env"][-1][0] * m["rate_hz"] // 1000000)
    noise["fold"] = min(noise["fold"], span // 2)
    if s["len"] < 400:
        for layer in s["layers"]:
            layer["hits"] = [[0, layer["hits"][0][1]]]
        s.pop("split", None) if s["len"] < 2 else None
    p = tmp_path / "model.json"
    p.write_text(json.dumps(m))
    assert sfx_check.load_model(str(p)) is not None
    d = tmp_path / "t"
    d.mkdir()
    table = bytes(128 + (i % 7) - 3 for i in range(s["len"]))
    if "split" in s:
        (d / "s_1.u8").write_bytes(table[:s["split"]])
        (d / "s_2.u8").write_bytes(table[s["split"]:])
    else:
        (d / "s.u8").write_bytes(table)
    assert sfx_check.main([str(p), str(d)]) in (0, 1, 2)


def burst_model(seed=1):
    """A 20 ms burst: 400 samples, one hit, short fades the generator needs."""
    return {"format": 1, "rate_hz": RATE, "ceiling_steps": 90, "sounds": {"burst": {
        "len": 400, "peak_steps": 60, "layers": [
            {"kind": "noise", "seed": seed, "level_mdb": 0, "hits": [[0, 0]],
             "env": [[0, -60000], [1000, 0], [19000, 0], [20000, -60000]],
             "bands": [[500, 0], [6000, 0]]}]}}}


def test_short_last_frame_joins_a_single_frame():
    sfx_check = _import_check()
    assert sfx_check.to_frames([1.0] * 12, 10) == [12.0]


def test_one_hit_spectrum_sums_to_its_power():
    sfx_check = _import_check()
    m = burst_model()
    sound = m["sounds"]["burst"]
    layer = sound["layers"][0]
    spec, _ = sfx_check.noise_layer(layer, sound, RATE, 512)
    rh = sfx_check.autocorr(sfx_check.fir_taps(layer["bands"], RATE))
    rg = sfx_check.autocorr(sfx_check.hit_gain(layer, RATE))
    assert sum(spec) == pytest.approx(rh[0] * rg[0], rel=1e-6)


@pytest.mark.parametrize("seed", range(1, 13))
def test_short_burst_passes_for_every_seed(tmp_path, seed):
    p, d = dump(tmp_path, burst_model(seed))
    r = run(CHECK, p, d)
    assert r.returncode == 0, r.stdout + r.stderr


def comb_model(repeat):
    return {"format": 1, "rate_hz": RATE, "ceiling_steps": 90, "sounds": {"comb": {
        "len": 8000, "peak_steps": 60, "layers": [
            {"kind": "noise", "seed": 5, "level_mdb": 0, "repeat": repeat,
             "hits": [[0, 0], [10, 0]],
             "env": [[0, -60000], [5000, 0], [395000, 0], [400000, -60000]],
             "bands": [[800, -30000], [900, 0], [1100, 0], [1200, -30000]]}]}}}


def test_repeated_hits_close_together_pass(tmp_path):
    p, d = dump(tmp_path, comb_model(True))
    r = run(CHECK, p, d)
    assert r.returncode == 0, r.stdout + r.stderr
    p2 = tmp_path / "plain.json"
    p2.write_text(json.dumps(comb_model(False)))
    r = run(CHECK, p2, d)
    assert r.returncode == 1, r.stdout + r.stderr


@pytest.mark.parametrize("seed", range(1, 11))
def test_narrow_noise_gets_long_frames(tmp_path, seed):
    m = {"format": 1, "rate_hz": RATE, "ceiling_steps": 90, "sounds": {"narrow": {
        "len": RATE, "peak_steps": 60, "layers": [
            {"kind": "noise", "seed": seed, "level_mdb": 0, "hits": [[0, 0]],
             "env": [[0, -60000], [10000, 0], [990000, 0], [1000000, -60000]],
             "bands": [[1900, -40000], [2000, 0], [2200, 0], [2300, -40000]]}]}}}
    p, d = dump(tmp_path, m)
    r = run(CHECK, p, d)
    assert r.returncode == 0, r.stdout + r.stderr
    ms = float(re.search(r"frames (\d+\.\d) ms", r.stdout).group(1))
    assert ms > 5.0, r.stdout


@pytest.mark.parametrize("seed", range(1, 6))
def test_short_noise_loop_gets_wide_bands(tmp_path, seed):
    m = {"format": 1, "rate_hz": RATE, "ceiling_steps": 90, "sounds": {"hiss": {
        "len": RATE // 10, "peak_steps": 30, "loop": True, "layers": [
            {"kind": "noise", "seed": seed, "level_mdb": 0, "hits": [[0, 0]],
             "env": [[0, 0], [100000, 0]],
             "bands": [[500, 0], [6000, 0]]}]}}}
    p, d = dump(tmp_path, m)
    r = run(CHECK, p, d)
    assert r.returncode == 0, r.stdout + r.stderr
    hz = int(re.search(r"bands (\d+) Hz", r.stdout).group(1))
    assert hz > 250, r.stdout


def test_2ms_tick_passes_as_thin_and_checks_its_level(tmp_path):
    m = {"format": 1, "rate_hz": RATE, "ceiling_steps": 90, "sounds": {"tick": {
        "len": 40, "peak_steps": 40, "layers": [
            {"kind": "noise", "seed": 2, "level_mdb": 0, "hits": [[0, 0]],
             "env": [[0, -60000], [300, 0], [1700, 0], [2000, -60000]],
             "bands": [[500, 0], [6000, 0]]}]}}}
    p, d = dump(tmp_path, m)
    r = run(CHECK, p, d)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "thin 1 bands, 1 frames" in r.stdout, r.stdout

    def spike(data):
        data[:] = bytes([128] * len(data))
        data[20] = 128 + 40
    _rewrite(os.path.join(d, "tick.u8"), spike)
    r = run(CHECK, p, d)
    assert r.returncode == 1, r.stdout + r.stderr
    assert re.search(r"level \d+\.\d\d dB outside", r.stdout), r.stdout


def wide_tick(bands, seed=2):
    """A 2 ms noise tick at 48 kHz: two thin bands under 15 kHz."""
    return {"format": 1, "rate_hz": 48000, "ceiling_steps": 90, "sounds": {"tick": {
        "len": 96, "peak_steps": 40, "layers": [
            {"kind": "noise", "seed": seed, "level_mdb": 0, "hits": [[0, 0]],
             "env": [[0, -60000], [300, 0], [1700, 0], [2000, -60000]],
             "bands": bands}]}}}


LOW_ONLY = [[500, 0], [3000, 0], [3500, -60000], [14000, -60000]]
HIGH_TOO = [[500, 0], [3000, 0], [3500, -60000], [9000, -60000], [9500, -15000],
            [14000, -15000]]


@pytest.mark.parametrize("seed", range(1, 13))
def test_thin_tick_passes_for_every_seed(tmp_path, seed):
    p, d = dump(tmp_path, wide_tick(LOW_ONLY, seed))
    r = run(CHECK, p, d)
    assert r.returncode == 0, r.stdout + r.stderr
    assert re.search(r"thin \d bands, 1 frames", r.stdout), r.stdout


def test_thin_band_too_strong_fails_beyond_chance(tmp_path):
    p, _ = dump(tmp_path, wide_tick(LOW_ONLY), "a.json", "a")
    _, d = dump(tmp_path, wide_tick(HIGH_TOO), "b.json", "b")
    r = run(CHECK, p, d)
    assert r.returncode == 1, r.stdout + r.stderr
    assert re.search(r"FAIL band at \d+ Hz is \+\d+\.\d\d dB off, beyond chance$",
                     r.stdout.strip()), r.stdout


@pytest.mark.parametrize("x", [0.01, 0.5, 1.0, 2.0, 5.0, 30.0])
def test_incomplete_gamma_known_values(x):
    sfx_check = _import_check()
    assert sfx_check.gamma_p(1.0, x) == pytest.approx(1.0 - math.exp(-x), rel=1e-9, abs=1e-12)
    assert sfx_check.gamma_p(0.5, x) == pytest.approx(math.erf(math.sqrt(x)), rel=1e-9, abs=1e-12)
    for a in (0.5, 1.0, 3.0, 50.0):
        assert sfx_check.gamma_p(a, x) + sfx_check.gamma_q(a, x) == pytest.approx(1.0, abs=1e-12)


def test_incomplete_gamma_median_near_its_shape():
    sfx_check = _import_check()
    assert sfx_check.gamma_p(10000.0, 10000.0) == pytest.approx(0.5, abs=0.01)


@pytest.mark.parametrize("n", [1, 2, 7, 100, 2049])
def test_dft_of_any_length_matches_a_direct_sum(n):
    sfx_check = _import_check()
    x = [complex(math.sin(1.3 * i + 0.2), math.cos(0.7 * i * i)) for i in range(n)]
    got = sfx_check.dft(x)
    for k in sorted({0, 1 % n, n // 2, n - 1}):
        want = sum(v * complex(math.cos(2 * math.pi * k * i / n), -math.sin(2 * math.pi * k * i / n))
                   for i, v in enumerate(x))
        assert abs(got[k] - want) <= 1e-9 * max(1.0, abs(want)) * n, (n, k)


def odd_loop_model(seed):
    """A 2049-sample noise loop with one hit at sample 1000."""
    n = 2049
    return {"format": 1, "rate_hz": RATE, "ceiling_steps": 90, "sounds": {"odd": {
        "len": n, "peak_steps": 30, "loop": True, "layers": [
            {"kind": "noise", "seed": seed, "level_mdb": 0, "hits": [[1000, 0]],
             "fold": 100, "env": [[0, 0], [(n + 100) * 1000000 // RATE, 0]],
             "bands": [[500, 0], [6000, 0]]}]}}}


@pytest.mark.parametrize("seed", range(1, 13))
def test_odd_loop_with_a_late_hit_passes(tmp_path, seed):
    p, d = dump(tmp_path, odd_loop_model(seed))
    r = run(CHECK, p, d)
    assert r.returncode == 0, r.stdout + r.stderr


def test_one_sample_loop_of_many_hits_is_quick(tmp_path):
    m = {"format": 1, "rate_hz": RATE, "ceiling_steps": 90, "sounds": {"dot": {
        "len": 1, "peak_steps": 1, "loop": True, "layers": [
            {"kind": "noise", "seed": 3, "level_mdb": -20000, "repeat": True,
             "hits": [[0, 0]] * 64, "env": [[0, 0], [500 * 1000000 // RATE, 0]],
             "bands": [[500, 0], [6000, 0]]}]}}}
    p, d = dump(tmp_path, m)
    t = time.monotonic()
    r = run(CHECK, p, d)
    assert time.monotonic() - t < 10.0
    assert r.returncode == 0, r.stdout + r.stderr


def cut_model(repeat):
    """Two hits, the second cut by the table end."""
    return {"format": 1, "rate_hz": RATE, "ceiling_steps": 90, "sounds": {"cut": {
        "len": 6000, "peak_steps": 60, "layers": [
            {"kind": "noise", "seed": 5, "level_mdb": 0, "repeat": repeat,
             "hits": [[0, 0], [4000, 0]],
             "env": [[0, -60000], [5000, 0], [90000, 0], [95000, -60000], [200000, -60000]],
             "bands": [[500, 0], [6000, 0]]}]}}}


def test_cut_repeated_hits_skip_the_bands(tmp_path):
    p, d = dump(tmp_path, cut_model(True))
    r = run(CHECK, p, d)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "bands not judged, repeated hits cut by the table end: ok" in r.stdout, r.stdout
    p, d = dump(tmp_path, cut_model(False), "b.json", "b")
    r = run(CHECK, p, d)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "not judged" not in r.stdout, r.stdout


@pytest.mark.parametrize("seed", range(1, 9))
def test_single_hit_cut_by_the_end_passes(tmp_path, seed):
    m = burst_model(seed)
    m["sounds"]["burst"]["len"] = 250
    # Silent past 12 ms, so the cut end is quiet as the generator wants.
    m["sounds"]["burst"]["layers"][0]["env"] = [
        [0, -60000], [1000, 0], [11000, 0], [12000, -60000], [20000, -60000]]
    r = run(CHECK, *dump(tmp_path, m))
    assert r.returncode == 0, r.stdout + r.stderr


def test_too_costly_sound_gives_2(tmp_path):
    n = 262144
    m = {"format": 1, "rate_hz": 8000, "ceiling_steps": 90, "sounds": {"big": {
        "len": n, "peak_steps": 40, "layers": [
            {"kind": "noise", "seed": 1, "level_mdb": 0, "hits": [[k, 0] for k in range(64)],
             "env": [[0, 0], [n * 1000000 // 8000, 0]],
             "bands": [[100, 0], [3000, 0]]}]}}}
    p = tmp_path / "model.json"
    p.write_text(json.dumps(m))
    (tmp_path / "big.u8").write_bytes(bytes([128]) * n)
    t = time.monotonic()
    r = run(CHECK, p, tmp_path)
    assert time.monotonic() - t < 10.0
    assert r.returncode == 2, r.stdout + r.stderr
    assert re.search(r"sounds\.big: too costly to check, \d+ steps, the limit is 200000000",
                     r.stderr), r.stderr
