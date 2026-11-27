"""Unit tests for tools/sfx_table.py on small made-up models."""

import copy
import json
import math
import os
import random
import shutil
import struct
import subprocess
import sys
import wave
import zlib

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOL = os.path.join(ROOT, "tools", "sfx_table.py")
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "tests"))
import sfx_table as T  # noqa: E402
from harness.env import minimal_env  # noqa: E402


def tone_model(**sound):
    """A 2000 Hz sine at 8000 Hz: a quarter turn per sample."""
    s = {"len": 8, "peak_steps": 8, "layers": [
        {"kind": "tone", "hits": [[0, 0]], "len": 8, "pitch_mhz": [2000000, 2000000],
         "level_mdb": 0, "partials": [[10000, 0]]}]}
    s.update(sound)
    return {"format": 1, "rate_hz": 8000, "ceiling_steps": 90, "sounds": {"beep": s}}


def noise_model(loop=False, length=2000, fold=0, peak=20):
    layer = {"kind": "noise", "seed": 7, "level_mdb": -6000, "hits": [[0, 0]],
             "env": [[0, -40000], [2000, 0], [40000, 0], [52000, -40000]],
             "bands": [[500, -12000], [2000, 0], [6000, -3000], [9000, -30000]]}
    if loop:
        n = T.ceil_div(52000 * 20000, 1000000)
        layer["env"] = [[0, 0], [52000, 0]]
        layer["fold"] = fold
        length = n - fold
    sound = {"len": length, "peak_steps": peak, "layers": [layer]}
    if loop:
        sound["loop"] = True
    return {"format": 1, "rate_hz": 20000, "ceiling_steps": 90, "sounds": {"hiss": sound}}


def run(*args):
    return subprocess.run([sys.executable, "-I", TOOL, *map(str, args)],
                          capture_output=True, text=True, env=minimal_env())


def write_model(tmp_path, model, name="model.json"):
    p = tmp_path / name
    p.write_text(json.dumps(model))
    return p


# ---------------------------------------------------------------------------
# Arithmetic
# ---------------------------------------------------------------------------

def test_sine_exact_points():
    assert T.sin(0.0) == 0.0
    assert T.cos(0.0) == 1.0
    assert T.sin(T.HALF_PI) == 1.0
    assert T.cos(T.PI) == -1.0
    assert T.sin(-T.HALF_PI) == -1.0
    # The double nearest pi is below pi by 1.2246467991473532e-16.
    assert abs(T.sin(T.PI) - 1.2246467991473532e-16) < 1e-30
    assert abs(T.sin(T.PI / 6) - 0.5) <= 1.2e-16
    with pytest.raises(ValueError):
        T.sin(2e6)


def test_sine_error_bound():
    rng = random.Random(5)
    worst = 0.0
    for i in range(20000):
        x = rng.uniform(-1.6e6, 1.6e6) if i % 2 else rng.uniform(-8.0, 8.0)
        worst = max(worst, abs(T.sin(x) - math.sin(x)), abs(T.cos(x) - math.cos(x)))
    assert worst < 5e-16


def test_sine_gives_the_library_table():
    # floor(32767 * sin(2 pi k / 1024) + 0.5), 2048 bytes little-endian.
    data = b"".join(struct.pack("<h", math.floor(32767 * T.sin(2 * T.PI * k / 1024) + 0.5))
                    for k in range(1024))
    assert zlib.crc32(data) == 0x05067686


def test_log_identities():
    assert T.ln(1.0) == 0.0
    ln2 = 0.6931471805599453
    for k in (-60, -1, 1, 2, 10, 300):
        assert abs(T.ln(math.ldexp(1.0, k)) - k * ln2) <= 4 * abs(k) * 1.2e-16 + 2e-16
    assert abs(T.LN10 - 2.302585092994046) <= 4.5e-16
    for x in (3.0, 0.1, 12345.678, 1e-200):
        assert abs(T.ln(x) - math.log(x)) <= 4 * math.ulp(math.log(x)) + 2e-16
    with pytest.raises(ValueError):
        T.ln(0.0)


def test_exp_identities():
    assert T.exp(0.0) == 1.0
    assert T.exp(-800.0) == 0.0
    for k in (-50, -1, 1, 7, 40):
        assert T.exp(k * 0.6931471805599453) == pytest.approx(math.ldexp(1.0, k), rel=7e-16)
    for x in (0.5, 2.0, 37.5, 0.001):
        assert abs(T.exp(T.ln(x)) - x) <= 4 * math.ulp(x)
    for x in (0.5, 2.0, 37.5, -12.25):
        assert abs(T.exp(x) - math.exp(x)) <= 3 * math.ulp(math.exp(x))


def test_sqrt_identities():
    for n in (1, 4, 9, 144, 65536, 12345 * 12345):
        assert T.sqrt(float(n)) == float(math.isqrt(n))
    assert T.sqrt(0.0) == 0.0
    assert T.sqrt(math.ldexp(1.0, -40)) == math.ldexp(1.0, -20)
    for x in (2.0, 3.0, 12.0, 1e-300, 1e300):
        assert abs(T.sqrt(x) - math.sqrt(x)) <= math.ulp(math.sqrt(x))


def test_round_half_away():
    assert [T.round_half_away(v) for v in (0.5, 1.5, 2.5, -0.5, -2.5, 0.49999999999999994, 2.4)] \
        == [1, 2, 3, -1, -3, 0, 2]


# ---------------------------------------------------------------------------
# Synthesis
# ---------------------------------------------------------------------------

def test_tone_exact_samples():
    m = tone_model()
    assert T.render_sound(m, "beep") == [128, 136, 128, 120, 128, 136, 128, 120]
    # A 500 us attack at 8000 Hz is 4 samples: gains 0.125, 0.375, 0.625, 0.875.
    m["sounds"]["beep"]["layers"][0]["attack_us"] = 500
    assert T.render_sound(m, "beep") == [128, 131, 128, 121, 128, 136, 128, 120]


def test_tone_skips_partials_at_half_the_rate():
    m = tone_model()
    m["sounds"]["beep"]["layers"][0]["partials"] = [[10000, 0], [20000, 0]]
    assert T.render_sound(m, "beep") == [128, 136, 128, 120, 128, 136, 128, 120]


def test_noise_first_values():
    expect = {1: (0x5C5329B6, [0x15DB5584, 0x9216AC11, 0xB50154BB, 0x6D591570]),
              2: (0x787289EE, [0x00A4F469, 0x5B1AF4DD, 0xB5AB95BD, 0x24969053])}
    for seed, (start, states) in expect.items():
        s = T.start_state(seed, 0)
        assert s == start
        got = []
        for _ in range(4):
            s = T.xorshift32(s)
            got.append(s)
        assert got == states
        w = T.white(T.start_state(seed, 0), 4)
        assert w == [(((v >> 8) & 0xFFFF) - 32768) / T.NOISE_SCALE for v in states]
    assert T.white(T.start_state(1, 0), 1)[0] * T.NOISE_SCALE == 23381
    # A zero start becomes 0x1234567 before the four steps.
    seed = (-0x9E3779B9 * pow(2654435761, -1, 1 << 32)) % (1 << 32)
    s = 0x1234567
    for _ in range(4):
        s = T.xorshift32(s)
    assert T.start_state(seed, 0) == s


def test_filter_unit_power_and_symmetry():
    h = T.fir_design([[500, -12000], [2000, 0], [6000, -3000], [9000, -30000]], 20000)
    assert len(h) == 511
    assert all(h[i] == h[510 - i] for i in range(511))
    assert abs(math.fsum(v * v for v in h) - 1.0) < 1e-12
    assert T.fir_design([[19000, 0], [19001, 0]], 37500) is None


def test_fast_filter_matches_the_definition_bit_for_bit():
    h = T.fir_design([[300, 0], [5000, -6000], [8000, -20000]], 20000)
    w = T.white(T.start_state(3, 0), 700 + 510)
    assert T.convolve(h, w, 700) == T.convolve_direct(h, w, 700)
    m = noise_model(length=1100)
    assert T.render_float(m, "hiss") == T.render_float(m, "hiss", direct=True)


def test_envelope_segment_rule():
    # 10000 Hz: one sample per 100 us. Points at samples 0, 2 and 5.
    pts = [[0, 0], [200, -6000], [500, 0]]
    g = T.env_gain(pts, 8, 10000)
    assert g[0] == 1.0
    assert g[1] == T.db_gain(-3.0)
    assert g[2] == T.db_gain(-6.0)      # a whole-sample point opens the next segment
    assert g[4] == T.db_gain(-6.0 + 6.0 * 2 / 3)
    assert g[5:] == [0.0, 0.0, 0.0]     # silent after the last point
    held = T.env_gain(pts, 8, 10000, hold_last=True)
    assert held[5:] == [1.0, 1.0, 1.0]
    # A point at a fraction: 250 us is sample 2.5, so samples 0 to 2 are the first segment.
    g = T.env_gain([[0, 0], [250, -5000], [500, -5000]], 6, 10000)
    assert g[2] == T.db_gain(-5.0 * 2 / 2.5)
    assert g[3] == T.db_gain(-5.0)
    # Silent before the first point.
    assert T.env_gain([[300, 0], [500, 0]], 6, 10000)[:3] == [0.0, 0.0, 0.0]


def test_fold_constant_power():
    m = noise_model(loop=True, fold=100)
    layer = m["sounds"]["hiss"]["layers"][0]
    g = T.noise_gain(layer, 20000)
    n = len(g)
    assert g[0] == T.sin(T.HALF_PI * 0.5 / 100)
    for i in range(100):
        # The tail of a hit falls on its own head in the loop.
        assert abs(g[i] ** 2 + g[n - 100 + i] ** 2 - 1.0) < 1e-15
    assert g[100:n - 100] == [1.0] * (n - 200)


def test_loop_join():
    m = noise_model(loop=True, fold=200)
    data = T.render_sound(m, "hiss")
    assert T.check_sound(m, "hiss", data) == []
    steps = [abs(a - b) for a, b in zip(data, data[1:])]
    assert abs(data[-1] - data[0]) <= max(steps)
    # A table with a jump at its join is refused.
    bad = [128] * 50 + [128 + 20] + [128] * 49
    bad[0] = 128 - 20
    bad[-1] = 128 + 20
    assert any("join" in f for f in T.check_sound(m, "hiss", bad))


def test_loop_tone_moves_to_whole_cycles():
    # 2000.5 Hz over 800 samples at 8000 Hz is 200.05 cycles: a move of 0.025 %.
    m = tone_model(len=800, loop=True, peak_steps=10)
    lay = m["sounds"]["beep"]["layers"][0]
    lay["len"] = 800
    lay["pitch_mhz"] = [2000500, 2000500]
    data = T.render_sound(m, "beep")
    assert T.check_sound(m, "beep", data) == []
    # 2002 Hz is 200.2 cycles, 0.1 % from 200: refused.
    lay["pitch_mhz"] = [2002000, 2002000]
    with pytest.raises(T.ModelError, match="above 0.05 %"):
        T.render_sound(m, "beep")


def test_peak_steps_hit_exactly():
    for peak in (1, 17, 31):
        m = noise_model(peak=peak)
        m["sounds"]["hiss"]["peak_steps"] = peak
        data = T.render_sound(m, "hiss")
        assert max(abs(b - 128) for b in data) == peak
        steps = T.render_steps(m, "hiss")
        assert max(abs(v) for v in steps) == pytest.approx(peak, abs=1e-12)


def test_split(tmp_path):
    m = noise_model(loop=True, fold=200)
    m["sounds"]["hiss"]["split"] = 300
    whole = bytes(T.render_sound(m, "hiss"))
    built = T.build(m)
    parts = T.tables(m, "hiss", built["hiss"])
    assert [p[0] for p in parts] == ["hiss_1", "hiss_2"]
    assert len(parts[0][1]) == 300 and parts[0][1] + parts[1][1] == whole
    p = write_model(tmp_path, m)
    r = run(p, "--c", tmp_path / "t.c", "--h", tmp_path / "t.h", "--prefix", "x_",
            "--dump", tmp_path / "d")
    assert r.returncode == 0, r.stderr
    h = (tmp_path / "t.h").read_text()
    assert "#define X_HISS_1_LEN 300u" in h and "extern const uint8_t x_hiss_2[X_HISS_2_LEN];" in h
    assert (tmp_path / "d" / "hiss_1.u8").read_bytes() == parts[0][1]
    assert (tmp_path / "d" / "hiss_2.u8").read_bytes() == parts[1][1]


# ---------------------------------------------------------------------------
# Refusals
# ---------------------------------------------------------------------------

def _bad(mutate):
    m = noise_model()
    m["sounds"]["tick"] = tone_model()["sounds"]["beep"]
    m["sounds"]["tick"]["peak_steps"] = 8
    m["rate_hz"] = 8000
    m["sounds"]["hiss"]["layers"][0]["bands"] = [[500, -12000], [2000, 0], [3500, -3000]]
    mutate(m)
    return m


def _set(path, value):
    def f(m):
        obj = m
        for k in path[:-1]:
            obj = obj[k]
        if value is DELETE:
            del obj[path[-1]]
        else:
            obj[path[-1]] = value
    return f


DELETE = object()
HISS = ("sounds", "hiss")
HL = HISS + ("layers", 0)
TICK = ("sounds", "tick")
TL = TICK + ("layers", 0)

REFUSALS = [
    (_set(("extra",), 1), "extra: unknown field"),
    (_set(("format",), 2), "format: must be 1"),
    (_set(("rate_hz",), 37500.0), "rate_hz: must be a whole number"),
    (_set(("rate_hz",), 100), "rate_hz: must be 8000 to 48000, not 100"),
    (_set(("ceiling_steps",), 128), "ceiling_steps: must be 1 to 127"),
    (_set(("sounds",), {}), "sounds: must be an object with at least one sound"),
    (_set(("sounds", "Bad-Name"), tone_model()["sounds"]["beep"]), "sound 'Bad-Name': the name"),
    (_set(HISS + ("len",), DELETE), "sound 'hiss': missing field 'len'"),
    (_set(HISS + ("color",), 1), "sound 'hiss'.color: unknown field"),
    (_set(HISS + ("peak_steps",), 91), "sound 'hiss'.peak_steps: must be 1 to 90, not 91"),
    (_set(HISS + ("loop",), 1), "sound 'hiss'.loop: must be true or false"),
    (_set(HISS + ("split",), 2000), "sound 'hiss'.split: must be 1 to 1999"),
    (_set(HISS + ("layers",), []), "sound 'hiss'.layers: must be a list of 1 to 16 layers"),
    (_set(HISS + ("layers",), [None] * 17), "must be a list of 1 to 16 layers"),
    (_set(HL + ("kind",), "square"), "layers[0].kind: must be \"tone\" or \"noise\""),
    (_set(HL + ("scatter",), [1, 2, 3, 4]), "layers[0].scatter: unknown field"),
    (_set(HL + ("hits",), [[0, 0, 0]]), "layers[0].hits[0]: must be a pair"),
    (_set(HL + ("hits",), [[2000, 0]]), "layers[0].hits[0][0]: must be 0 to 1999, not 2000"),
    (_set(HL + ("hits",), [[0, 0]] * 65), "layers[0].hits: must hold 1 to 64 points"),
    (_set(HL + ("level_mdb",), -6.5), "layers[0].level_mdb: must be a whole number"),
    (_set(HL + ("seed",), True), "layers[0].seed: must be a whole number"),
    (_set(HL + ("seed",), 1 << 32), "layers[0].seed: must be 0 to 4294967295"),
    (_set(HL + ("env",), [[0, 0]]), "layers[0].env: must hold 2 to 64 points, not 1"),
    (_set(HL + ("env",), [[100, 0], [50, 0]]), "layers[0].env[1][0]: must not come before"),
    (_set(HL + ("bands",), [[100 * i, 0] for i in range(25)]), "layers[0].bands: must hold 2 to 24 points, not 25"),
    (_set(HL + ("bands",), [[100, 0], [4001, 0]]), "layers[0].bands[1][0]: must be 0 to 4000"),
    (_set(HL + ("bands",), [[900, 0], [800, 0]]), "layers[0].bands[1][0]: must be above"),
    (_set(HL + ("bands",), [[0, 0], [1000, 0], [1000, -6000], [4000, -6000]]),
     "layers[0].bands[2][0]: must be above the point above it"),
    (_set(HL + ("fold",), 1000), "layers[0].fold: must be 0 to 208, not 1000"),
    (_set(HL + ("repeat",), "yes"), "layers[0].repeat: must be true or false"),
    (_set(TL + ("pitch_mhz",), [1000]), "layers[0].pitch_mhz: must be [from, to] in millihertz"),
    (_set(TL + ("pitch_mhz",), [0, 1000]), "layers[0].pitch_mhz[0]: must be 1 to 4000000"),
    (_set(TL + ("partials",), [[0, 0]]), "layers[0].partials[0][0]: must be 1 to 1000000"),
    (_set(TL + ("len",), 0), "layers[0].len: must be 1 to 262144, not 0"),
    (_set(TL + ("attack_us",), 2000), "layers[0].attack_us: must fit inside the tone's len"),
    (_set(TL + ("fold",), 3), "layers[0].fold: unknown field"),
]


@pytest.mark.parametrize("mutate,message", REFUSALS, ids=[m for _, m in REFUSALS])
def test_refusal(tmp_path, mutate, message):
    m = _bad(mutate)
    p = write_model(tmp_path, m)
    r = run(p, "--dump", tmp_path / "d")
    assert r.returncode == 2, r.stderr
    assert message in r.stderr
    assert "Traceback" not in r.stderr
    assert not (tmp_path / "d").exists()


def test_good_base_of_the_refusals(tmp_path):
    p = write_model(tmp_path, _bad(lambda m: None))
    r = run(p, "--dump", tmp_path / "d")
    assert r.returncode == 0, r.stderr


@pytest.mark.parametrize("text,message", [
    ("{", "not valid JSON"),
    ('{"format": 1, "format": 1}', "field 'format' appears twice"),
    ('{"format": NaN}', "NaN is not a number of the format"),
])
def test_refusal_of_bad_json(tmp_path, text, message):
    p = tmp_path / "m.json"
    p.write_text(text)
    r = run(p, "--dump", tmp_path)
    assert r.returncode == 2 and message in r.stderr and "Traceback" not in r.stderr


def test_every_fault_has_its_line(tmp_path):
    m = _bad(lambda m: None)
    m["rate_hz"] = 1.5
    m["sounds"]["hiss"]["peak_steps"] = 0
    r = run(write_model(tmp_path, m), "--dump", tmp_path)
    lines = r.stderr.strip().splitlines()
    assert r.returncode == 2 and len(lines) == 2
    assert all(line.startswith("sfx_table.py: ") for line in lines)


def test_render_checks():
    # The checks after rendering, one per fault.
    m = tone_model()
    m["sounds"]["beep"]["peak_steps"] = 12   # the last sample ends 12 steps from rest
    data = T.render_sound(m, "beep")
    assert T.check_sound(m, "beep", data) == [
        "sound 'beep': the last sample is -12 steps from 128, more than 8"]
    first = [140] + data[1:]
    assert "the first sample is +12 steps from 128, more than 2" in T.check_sound(m, "beep", first)[0]
    loud = noise_model(loop=True, fold=100, peak=40)
    assert any("a loop's peak_steps must be 32 or less, not 40" in f
               for f in T.check_sound(loud, "hiss", T.render_sound(loud, "hiss")))
    over = [128 + 95] + data[1:]
    assert any("past ceiling_steps 90" in f for f in T.check_sound(m, "beep", over))


def test_cli_refuses_a_failed_check(tmp_path):
    m = tone_model(peak_steps=12)
    r = run(write_model(tmp_path, m), "--dump", tmp_path / "d")
    assert r.returncode == 2
    assert "the last sample is -12 steps from 128, more than 8" in r.stderr


def test_usage_errors(tmp_path):
    p = write_model(tmp_path, tone_model())
    c, h, x = tmp_path / "a.c", tmp_path / "a.h", tmp_path / "x"
    for args in ((p,), (p, "--c", c), (p, "--full"), (p, "--check", "--dump", x),
                 (p, "--c", c, "--h", h, "--prefix", "Bad"),
                 (p, "--c", c, "--h", h, "--prefix", "_x"),
                 (p, "--c", c, "--h", h, "--prefix", "__x"),
                 (p, "--c", c, "--h", h, "--prefix", "1x"),
                 (p, "--c", c, "--h", tmp_path / "1.h", "--prefix", "x_"),
                 (p, "--c", c, "--h", tmp_path / "_x.h", "--prefix", "x_"),
                 (p, "--c", c, "--h", tmp_path / "x_rate_hz", "--prefix", "x_"),
                 (tmp_path / "s\u00f6nd.json", "--c", c, "--h", h, "--prefix", "x_"),
                 (tmp_path / "a\nb.json", "--c", c, "--h", h, "--prefix", "x_")):
        r = run(*args)
        assert r.returncode == 64, args
    assert sorted(f.name for f in tmp_path.iterdir()) == ["model.json"]


@pytest.mark.parametrize("prefix,sound,h_name,bad", [
    ("f", "loat", "t.h", "float, a C keyword"),
    ("b", "ool", "t.h", "bool, a C keyword"),
    ("static_", "assert", "t.h", "static_assert, a C keyword"),
    ("thread_", "local", "t.h", "thread_local, a C keyword"),
    ("nullpt", "r", "t.h", "nullptr, a C keyword"),
    ("typ", "eof", "t.h", "typeof, a C keyword"),
    ("a", "sm", "t.h", "asm, a C keyword"),
    ("uint8_", "t", "t.h", "uint8_t, a name of <stdint.h>"),
    ("in", "t32_t", "t.h", "int32_t, a name of <stdint.h>"),
    ("x_", "beep", "uint8_max", "UINT8_MAX, a name of <stdint.h>"),
    ("x_", "beep", "size_max", "SIZE_MAX, a name of <stdint.h>"),
    ("x_", "beep", "x_beep_peak", "X_BEEP_PEAK, already given by"),
    ("x_", "beep", "x_model_sha256", "X_MODEL_SHA256, already given by"),
])
def test_bad_c_names_are_refused(tmp_path, prefix, sound, h_name, bad):
    m = tone_model()
    m["sounds"] = {sound: m["sounds"]["beep"]}
    p = write_model(tmp_path, m)
    c, h = tmp_path / "t.c", tmp_path / h_name
    r = run(p, "--c", c, "--h", h, "--prefix", prefix)
    assert r.returncode == 64, r.stderr
    assert bad in r.stderr and "Traceback" not in r.stderr
    assert sorted(f.name for f in tmp_path.iterdir()) == ["model.json"]


def test_bad_c_names_split_and_reserved():
    m = tone_model(split=4)
    m["sounds"] = {"loat_1": m["sounds"]["beep"]}
    faults = T.c_name_faults(T.c_names(m, {"loat_1": bytes(8)}, "f", "t.h"))
    assert faults == []
    m["sounds"] = {"ool": tone_model()["sounds"]["beep"]}
    assert T.c_name_faults(T.c_names(m, {"ool": bytes(8)}, "b", "t.h")) == [
        "--prefix 'b' and sound 'ool' give bool, a C keyword"]
    assert T.c_name_faults([("_X", "here")]) == ["here give _X, a reserved C name"]
    assert T.c_name_faults([("A", "one"), ("A", "two")]) == ["two give A, already given by one"]


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------

def _generate(tmp_path, model):
    p = write_model(tmp_path, model)
    c, h = tmp_path / "sfx_table.c", tmp_path / "sfx_table.h"
    r = run(p, "--c", c, "--h", h, "--prefix", "demo_sfx_")
    assert r.returncode == 0, r.stderr
    return p, c, h


def _two_sounds():
    m = noise_model()
    m["sounds"]["beep"] = copy.deepcopy(tone_model()["sounds"]["beep"])
    m["sounds"]["beep"]["layers"][0]["pitch_mhz"] = [1000000, 1000000]
    m["sounds"]["beep"]["len"] = 9
    return m


def test_generated_files(tmp_path):
    m = _two_sounds()
    p, c, h = _generate(tmp_path, m)
    sha = __import__("hashlib").sha256(p.read_bytes()).hexdigest()
    ht, ct = h.read_text(), c.read_text()
    assert ht.startswith("/*\n * Generated file. Do not edit.\n * Source: model.json\n")
    assert f'#define DEMO_SFX_MODEL_SHA256 "{sha}"' in ht
    assert f"#define DEMO_SFX_TOOL_REVISION {T.REVISION}" in ht
    data = T.build(m)["hiss"]
    assert f"#define DEMO_SFX_HISS_LEN {len(data)}u" in ht
    assert f"#define DEMO_SFX_HISS_CRC32 0x{zlib.crc32(data):08x}u" in ht
    assert '#include "sfx_table.h"' in ct
    assert "const uint8_t demo_sfx_beep[DEMO_SFX_BEEP_LEN] = {" in ct
    assert str(ROOT) not in ht + ct


def test_check_catches_a_changed_model_and_table(tmp_path):
    p, c, h = _generate(tmp_path, _two_sounds())
    args = ("--c", c, "--h", h, "--prefix", "demo_sfx_", "--check")
    assert run(p, *args).returncode == 0
    assert run(p, *args, "--full").returncode == 0
    p.write_text(p.read_text() + "\n")
    r = run(p, *args)
    assert r.returncode == 1 and "the model changed" in r.stderr
    p.write_text(p.read_text()[:-1])
    ct = c.read_text()
    c.write_text(ct.replace("0x80,", "0x81,", 1))
    assert run(p, *args).returncode == 0          # the fast check reads only the header
    r = run(p, *args, "--full")
    assert r.returncode == 1 and "differs from a new render" in r.stderr
    c.write_text(ct)
    h.write_text(h.read_text().replace(f"TOOL_REVISION {T.REVISION}", "TOOL_REVISION 0"))
    r = run(p, *args)
    assert r.returncode == 1 and "made by revision 0" in r.stderr


def _no_temp_left(*folders):
    for d in folders:
        assert not [f.name for f in d.iterdir() if f.name.endswith(".tmp")], d


def _refusing_writes(tmp_path):
    probe = tmp_path / "probe"
    probe.mkdir()
    probe.chmod(0o555)
    try:
        (probe / "x").write_bytes(b"")
    except OSError:
        return True
    finally:
        probe.chmod(0o755)
    return False


def test_unwritable_source_leaves_both_old_files(tmp_path):
    if not _refusing_writes(tmp_path):
        pytest.skip("this user can write to a read-only folder, so the failure cannot be made")
    m = _two_sounds()
    p = write_model(tmp_path, m)
    cdir, hdir = tmp_path / "c", tmp_path / "h"
    cdir.mkdir()
    hdir.mkdir()
    c, h = cdir / "t.c", hdir / "t.h"
    args = ("--c", c, "--h", h, "--prefix", "demo_sfx_")
    assert run(p, *args).returncode == 0
    old_c, old_h = c.read_bytes(), h.read_bytes()
    m["sounds"]["beep"]["len"] = 10
    p = write_model(tmp_path, m)
    cdir.chmod(0o555)
    try:
        r = run(p, *args)
    finally:
        cdir.chmod(0o755)
    assert r.returncode == 1 and "Permission denied" in r.stderr
    assert len(r.stderr.strip().splitlines()) == 1
    assert c.read_bytes() == old_c and h.read_bytes() == old_h
    _no_temp_left(cdir, hdir)
    r = run(p, *args, "--check")
    assert r.returncode == 1 and "the model changed" in r.stderr


def test_unwritable_header_leaves_both_old_files(tmp_path):
    if not _refusing_writes(tmp_path):
        pytest.skip("this user can write to a read-only folder, so the failure cannot be made")
    m = _two_sounds()
    p = write_model(tmp_path, m)
    cdir, hdir = tmp_path / "c", tmp_path / "h"
    cdir.mkdir()
    hdir.mkdir()
    c, h = cdir / "t.c", hdir / "t.h"
    args = ("--c", c, "--h", h, "--prefix", "demo_sfx_")
    assert run(p, *args).returncode == 0
    old_c = c.read_bytes()
    m["sounds"]["beep"]["len"] = 10
    p = write_model(tmp_path, m)
    hdir.chmod(0o555)
    try:
        r = run(p, *args)
    finally:
        hdir.chmod(0o755)
    assert r.returncode == 1
    assert c.read_bytes() == old_c
    _no_temp_left(cdir, hdir)
    assert run(p, *args, "--check").returncode == 1


@pytest.mark.parametrize("fail_at", [1, 2])
def test_failed_rename_is_seen_as_stale(tmp_path, monkeypatch, capsys, fail_at):
    """A failed first or second rename, or a kill between the two."""
    m = _two_sounds()
    p, c, h = _generate(tmp_path, m)
    old_c, old_h = c.read_bytes(), h.read_bytes()
    m["sounds"]["beep"]["len"] = 10
    p = write_model(tmp_path, m)
    calls = []
    real = os.replace

    def replace(a, b):
        calls.append(b)
        if len(calls) == fail_at:
            raise OSError(5, "Input/output error", str(b))
        real(a, b)

    monkeypatch.setattr(T.os, "replace", replace)
    args = ["--c", str(c), "--h", str(h), "--prefix", "demo_sfx_"]
    assert T.main([str(p), *args]) == 1
    monkeypatch.setattr(T.os, "replace", real)
    assert [os.path.basename(x) for x in calls] == ["sfx_table.c", "sfx_table.h"][:fail_at]
    assert h.read_bytes() == old_h
    assert (c.read_bytes() == old_c) == (fail_at == 1)
    _no_temp_left(tmp_path)
    capsys.readouterr()
    assert T.main([str(p), *args, "--check"]) == 1
    assert "the model changed" in capsys.readouterr().err


def test_fast_check_reads_the_source_comment(tmp_path):
    p, c, h = _generate(tmp_path, _two_sounds())
    args = ("--c", c, "--h", h, "--prefix", "demo_sfx_", "--check")
    ct = c.read_text()
    sha = T.commented(ct)[0]
    c.write_text(ct.replace(sha, "0" * 64))
    r = run(p, *args)
    assert r.returncode == 1 and "does not name the same model" in r.stderr
    c.unlink()
    assert run(p, *args).returncode == 1


def test_failed_dump_leaves_no_new_table(tmp_path, monkeypatch):
    m = _two_sounds()
    p = write_model(tmp_path, m)
    d = tmp_path / "d"
    calls = []
    real = os.replace

    def replace(a, b):
        calls.append(b)
        if len(calls) == 2:
            raise OSError(5, "Input/output error", str(b))
        real(a, b)

    monkeypatch.setattr(T.os, "replace", replace)
    assert T.main([str(p), "--dump", str(d)]) == 1
    assert list(d.iterdir()) == []


def test_equal_bytes_on_two_runs(tmp_path):
    m = _two_sounds()
    a, b = tmp_path / "a", tmp_path / "b"
    a.mkdir()
    b.mkdir()
    _, ca, ha = _generate(a, m)
    _, cb, hb = _generate(b, m)
    assert ca.read_bytes() == cb.read_bytes() and ha.read_bytes() == hb.read_bytes()


def test_dump_and_wav(tmp_path):
    m = noise_model(loop=True, fold=100)
    m["sounds"]["beep"] = tone_model()["sounds"]["beep"]
    p = write_model(tmp_path, m)
    r = run(p, "--dump", tmp_path / "d", "--wav", tmp_path / "w")
    assert r.returncode == 0, r.stderr
    built = T.build(m)
    assert (tmp_path / "d" / "beep.u8").read_bytes() == built["beep"]
    with wave.open(str(tmp_path / "w" / "hiss.wav")) as w:
        assert (w.getnchannels(), w.getsampwidth(), w.getframerate()) == (1, 1, 20000)
        assert w.readframes(w.getnframes()) == built["hiss"] * 3   # a loop plays three times
    with wave.open(str(tmp_path / "w" / "beep.wav")) as w:
        assert w.getframerate() == 20000 and w.readframes(100) == built["beep"]


@pytest.mark.skipif(shutil.which("cc") is None, reason="no host C compiler")
def test_generated_c_compiles(tmp_path):
    m = _two_sounds()
    m["sounds"]["hiss"]["split"] = 700
    _, c, h = _generate(tmp_path, m)
    main = tmp_path / "main.c"
    main.write_text(
        '#include "sfx_table.h"\n'
        "_Static_assert(sizeof demo_sfx_hiss_1 == DEMO_SFX_HISS_1_LEN, \"len\");\n"
        "int main(void)\n{\n"
        "    return demo_sfx_beep[0] == 128 && DEMO_SFX_HISS_2_LEN == 1300u ? 0 : 1;\n}\n")
    exe = tmp_path / "a.out"
    r = subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-I", str(tmp_path),
                        str(c), str(main), "-o", str(exe)],
                       capture_output=True, text=True, env=minimal_env())
    assert r.returncode == 0, r.stderr
    assert subprocess.run([str(exe)], env=minimal_env()).returncode == 0


# ---------------------------------------------------------------------------
# Pinned bytes
# ---------------------------------------------------------------------------

def pinned_models():
    """Made-up models that use every feature: glides, partials, ramps, env,
    noise bands, repeat, fold, loops and split."""
    return [
        {"format": 1, "rate_hz": 22050, "ceiling_steps": 100, "sounds": {
            "chirp": {"len": 3000, "peak_steps": 70, "layers": [
                {"kind": "tone", "hits": [[0, 0], [1200, -4500]], "len": 1500,
                 "pitch_mhz": [440000, 3520000], "level_mdb": -3000,
                 "partials": [[10000, 0], [20000, -6000], [35000, -12000], [123457, -20000]],
                 "attack_us": 2000, "release_us": 15000,
                 "env": [[0, 0], [20000, -3000], [45000, -9000], [60000, -40000]]}]},
            "drop": {"len": 4000, "peak_steps": 50, "layers": [
                {"kind": "tone", "hits": [[0, 0]], "len": 4000,
                 "pitch_mhz": [9000000, 120000], "level_mdb": 0,
                 "partials": [[10000, 0], [30000, -9000], [70000, -15000]],
                 "attack_us": 1000, "release_us": 40000}]}}},
        {"format": 1, "rate_hz": 16000, "ceiling_steps": 90, "sounds": {
            "burst": {"len": 1800, "peak_steps": 60, "layers": [
                {"kind": "noise", "seed": 12345, "level_mdb": -2000, "hits": [[0, 0], [700, -6000]],
                 "env": [[0, -40000], [3000, 0], [30000, -8000], [60000, -45000]],
                 "bands": [[0, -30000], [400, -6000], [2500, 0], [5000, -9000], [8000, -40000]]},
                {"kind": "tone", "hits": [[0, -6000]], "len": 900,
                 "pitch_mhz": [1500000, 1500000], "level_mdb": -6000,
                 "partials": [[10000, 0], [25000, -10000]], "release_us": 20000}]},
            "ticks": {"len": 1600, "peak_steps": 40, "split": 700, "layers": [
                {"kind": "noise", "seed": 9, "level_mdb": 0, "repeat": True,
                 "hits": [[0, 0], [500, -3000], [1000, -6000]],
                 "env": [[0, -40000], [600, 0], [5000, -20000], [20000, -60000]],
                 "bands": [[1000, -10000], [3000, 0], [7000, -5000]]}]}}},
        {"format": 1, "rate_hz": 20000, "ceiling_steps": 60, "sounds": {
            "hum": {"len": 2000, "peak_steps": 24, "loop": True, "layers": [
                {"kind": "tone", "hits": [[0, 0]], "len": 2000,
                 "pitch_mhz": [120000, 120000], "level_mdb": -6000,
                 "partials": [[10000, 0], [20000, -6000], [30000, -12000]]}]},
            "fan": {"len": 1500, "peak_steps": 20, "loop": True, "layers": [
                {"kind": "noise", "seed": 77, "level_mdb": -3000, "fold": 100,
                 "hits": [[0, 0]], "env": [[0, 0], [80000, 0]],
                 "bands": [[0, -6000], [200, 0], [1500, -6000], [6000, -30000]]}]}}},
    ]


PINNED = {"chirp": 0x173421A4, "drop": 0xA16C7C0B, "burst": 0x37B3D822,
          "ticks": 0x4F711EC1, "hum": 0xD0AE90A3, "fan": 0xD43D1D14}


def test_pinned_bytes():
    # A change here changes a table: raise REVISION and say why.
    got = {}
    for m in pinned_models():
        for name, data in T.build(m).items():
            got[name] = zlib.crc32(data)
    assert got == PINNED


# ---------------------------------------------------------------------------
# Extreme models: they render or are refused, never a traceback
# ---------------------------------------------------------------------------

def _tone(n, pitch, mult, level=0, part=0, hit=0, **extra):
    layer = {"kind": "tone", "hits": [[0, hit]], "len": n, "pitch_mhz": pitch,
             "level_mdb": level, "partials": [[mult, part]]}
    layer.update(extra)
    return layer


def test_long_falling_glide_with_a_high_partial_is_refused(tmp_path):
    # 20 kHz down to 100 Hz over 100000 samples at 48 kHz with a 100x partial.
    m = {"format": 1, "rate_hz": 48000, "ceiling_steps": 127, "sounds": {"dive": {
        "len": 100000, "peak_steps": 100,
        "layers": [_tone(100000, [20000000, 100000], 1000000)]}}}
    assert T.sine_bound(100000, 20000000, 100000, 1000000, 48000) > T.SINE_LIMIT
    r = run(write_model(tmp_path, m), "--dump", tmp_path / "d")
    assert r.returncode == 2 and "Traceback" not in r.stderr
    assert r.stderr.count("\n") == 1
    assert "sound 'dive'.layers[0].partials: the sine of multiple 1000000" in r.stderr


def test_sine_bound_holds():
    # The bound is above the largest argument the renderer really reaches.
    for n, a, b, mult, rate in ((5000, 9000000, 120000, 70000, 22050),
                                (3000, 3000000, 2000, 10000, 8000),
                                (1, 4000000, 1, 10000, 8000),
                                (4000, 440000, 3520000, 30000, 16000)):
        f0, f1 = a / 1000.0, b / 1000.0
        ratio = T.exp(T.ln(f1 / f0) / n)
        f, phase, worst = f0, 0.0, 0.0
        for _ in range(n):
            if f * mult / 10000.0 < rate / 2:
                worst = max(worst, phase * mult / 10000.0)
            phase += T.TWO_PI * f / rate
            f *= ratio
        assert worst <= T.sine_bound(n, a, b, mult, rate)


EXTREMES = [
    # Longest tone at the highest rate, rising to the top with the highest partial.
    {"len": 262144, "peak_steps": 32, "loop": True,
     "layers": [_tone(262144, [1, 24000000], 1000000)]},
    # The same falling: refused by the sine bound.
    {"len": 262144, "peak_steps": 40,
     "layers": [_tone(262144, [24000000, 1], 1000000)]},
    # Lowest and highest levels everywhere.
    {"len": 2000, "peak_steps": 127,
     "layers": [_tone(2000, [1000000, 1000000], 10000, -120000, -120000, -120000,
                      attack_us=41666, release_us=41666),
                _tone(2000, [1000000, 1000000], 10000, 20000, 20000, 20000)]},
    # The longest envelope, 64 points, and a tone env held to the end.
    {"len": 3000, "peak_steps": 60,
     "layers": [_tone(3000, [24000000, 24000000], 10000,
                      env=[[i * 1000, -120000 if i % 2 else 20000] for i in range(64)]),
                {"kind": "noise", "seed": 4294967295, "level_mdb": 20000, "hits": [[0, 20000]],
                 "fold": 900, "repeat": True,
                 "env": [[i * 650, -120000 if i % 2 else 20000] for i in range(64)],
                 "bands": [[0, 20000], [24000, -120000]]}]},
]


@pytest.mark.parametrize("sound", EXTREMES, ids=range(len(EXTREMES)))
def test_extreme_models_render_or_are_refused(tmp_path, sound):
    m = {"format": 1, "rate_hz": 48000, "ceiling_steps": 127, "sounds": {"x": sound}}
    r = run(write_model(tmp_path, m), "--dump", tmp_path / "d")
    assert r.returncode in (0, 2) and "Traceback" not in r.stderr
    if r.returncode == 2:
        lines = r.stderr.strip().splitlines()
        assert lines and all(line.startswith("sfx_table.py: ") for line in lines)


@pytest.mark.skipif(shutil.which("cc") is None, reason="no host C compiler")
@pytest.mark.parametrize("prefix,h_name", [
    ("demo_sfx_", "sfx_table.h"), ("a", "t.h"), ("snd__", "sfx-table.h"),
    ("z9_", "my.sounds.h"), ("q_", "h1.h"), ("x_", "x_rate_hz.h")])
def test_generated_c_compiles_for_names(tmp_path, prefix, h_name):
    m = _two_sounds()
    p = write_model(tmp_path, m)
    c, h = tmp_path / "out.c", tmp_path / h_name
    r = run(p, "--c", c, "--h", h, "--prefix", prefix)
    assert r.returncode == 0, r.stderr
    main = tmp_path / "main.c"
    main.write_text(f'#include "{h_name}"\n#include "{h_name}"\n'
                    f"int main(void)\n{{\n    return {prefix}beep[0] == 128 ? 0 : 1;\n}}\n")
    r = subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-I", str(tmp_path),
                        str(c), str(main), "-o", str(tmp_path / "a.out")],
                       capture_output=True, text=True, env=minimal_env())
    assert r.returncode == 0, r.stderr
