"""Unit tests for tools/sfx_table.py on small made-up models."""

import copy
import json
import math
import os
import random
import re
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
    with pytest.raises(T.RangeFault):
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
    with pytest.raises(T.RangeFault):
        T.ln(0.0)


@pytest.mark.parametrize("f,x", [
    (T.sin, 1.7e6), (T.sin, -1.7e6), (T.sin, math.nan), (T.cos, math.inf),
    (T.ln, 0.0), (T.ln, -1.0), (T.ln, math.inf), (T.ln, math.nan),
    (T.exp, 710.0), (T.exp, math.inf), (T.exp, math.nan),
    (T.sqrt, -1.0), (T.sqrt, math.inf), (T.sqrt, math.nan)])
def test_functions_refuse_out_of_range(f, x):
    # The backstop: each function names itself and its argument.
    with pytest.raises(T.RangeFault, match=f.__name__):
        f(x)


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
    r = run(p, "--c", tmp_path / "sfx_t.c", "--h", tmp_path / "sfx_t.h", "--prefix", "x_sfx_",
            "--dump", tmp_path / "d")
    assert r.returncode == 0, r.stderr
    h = (tmp_path / "sfx_t.h").read_text()
    assert "#define X_SFX_HISS_1_LEN 300u" in h
    assert "extern const uint8_t x_sfx_hiss_2[X_SFX_HISS_2_LEN];" in h
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
    (_set(("rate_hz",), True), "rate_hz: must be a whole number"),
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
    (_set(HL + ("level_mdb",), False), "layers[0].level_mdb: must be a whole number"),
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
    m["rate_hz"] = True
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
    # The validator refuses it, before and whatever a render gives.
    assert any("a loop's peak_steps must be 32 or less, not 40" in f
               for f in T.check_model(loud))
    assert not any("32 or less" in f
                   for f in T.check_sound(loud, "hiss", T.render_sound(loud, "hiss")))
    quiet = noise_model(loop=True, fold=100, peak=40)
    quiet["sounds"]["hiss"]["layers"][0]["level_mdb"] = -120000
    quiet["sounds"]["hiss"]["layers"][0]["env"] = [[0, -120000], [52000, -120000]]
    assert any("a loop's peak_steps must be 32 or less, not 40" in f
               for f in T.check_model(quiet))
    over = [128 + 95] + data[1:]
    assert any("past ceiling_steps 90" in f for f in T.check_sound(m, "beep", over))


def test_cli_refuses_a_failed_check(tmp_path):
    m = tone_model(peak_steps=12)
    r = run(write_model(tmp_path, m), "--dump", tmp_path / "d")
    assert r.returncode == 2
    assert "the last sample is -12 steps from 128, more than 8" in r.stderr


def test_usage_errors(tmp_path):
    p = write_model(tmp_path, tone_model())
    c, h, x = tmp_path / "sfx_a.c", tmp_path / "sfx_a.h", tmp_path / "x"
    for args in ((p,), (p, "--c", c), (p, "--full"), (p, "--check", "--dump", x),
                 (p, "--c", c, "--h", h, "--prefix", "Bad"),
                 (p, "--c", c, "--h", h, "--prefix", "_x"),
                 (p, "--c", c, "--h", h, "--prefix", "__x"),
                 (p, "--c", c, "--h", h, "--prefix", "1x"),
                 (p, "--c", c, "--h", h, "--prefix", "x_"),
                 (p, "--c", c, "--h", h, "--prefix", "sfx"),
                 (p, "--c", c, "--h", h, "--prefix", "x__sfx_"),
                 (p, "--c", c, "--h", h, "--prefix", "_sfx_"),
                 (p, "--c", c, "--h", h, "--prefix", "sfx_x_"),
                 (p, "--c", c, "--h", h, "--prefix", "a" * 29 + "_sfx_"),
                 (p, "--c", tmp_path / "a.c", "--h", h, "--prefix", "sfx_"),
                 (p, "--c", c, "--h", tmp_path / "a.h", "--prefix", "sfx_"),
                 (p, "--c", c, "--h", tmp_path / "1.h", "--prefix", "x_sfx_"),
                 (p, "--c", c, "--h", tmp_path / "_x.h", "--prefix", "x_sfx_"),
                 (p, "--c", c, "--h", tmp_path / "x_rate_hz", "--prefix", "x_sfx_"),
                 (tmp_path / "s\u00f6nd.json", "--c", c, "--h", h, "--prefix", "x_sfx_"),
                 (tmp_path / "a\nb.json", "--c", c, "--h", h, "--prefix", "x_sfx_"),
                 (tmp_path / "x??=.json", "--c", c, "--h", h, "--prefix", "x_sfx_"),
                 (tmp_path / "a b.json", "--c", c, "--h", h, "--prefix", "x_sfx_"),
                 (tmp_path / "a*b.json", "--c", c, "--h", h, "--prefix", "x_sfx_"),
                 (tmp_path / "-m.json", "--c", c, "--h", h, "--prefix", "x_sfx_")):
        r = run(*args)
        assert r.returncode == 64, args
    assert sorted(f.name for f in tmp_path.iterdir()) == ["model.json"]


# Names the keyword list, the <stdint.h> patterns, the reserved-name check
# and the standard header list once refused, each by its own rule. The marker
# rule refuses them all: no such name ends in sfx_ or holds sfx.
OLD_NAME_CASES = [
    ("f", "loat"), ("b", "ool"), ("static_", "assert"), ("thread_", "local"),
    ("nullpt", "r"), ("typ", "eof"), ("a", "sm"), ("uint8_", "t"), ("in", "t32_t"),
    ("size_", "t"), ("_", "x"), ("__", "x"), ("uint8_max", "x"), ("size_max", "x"),
]
OLD_HEADER_CASES = ["stdint.h", "Stdint.h", "stdbit.h", "string.h", "unistd.h",
                    "pthread.h", "ar.h", "AR.h", "ucontext.h", "wordexp.h", "stddef.h"]


def test_old_name_lists_are_refused_by_the_marker_rule(tmp_path):
    p = write_model(tmp_path, tone_model())
    c, h = tmp_path / "sfx_t.c", tmp_path / "sfx_t.h"
    for prefix, sound in OLD_NAME_CASES:
        assert not T.PREFIX.match(prefix), prefix
        assert "sfx" not in (prefix + sound).lower()
        r = run(p, "--c", c, "--h", h, "--prefix", prefix)
        assert r.returncode == 64 and f"--prefix {prefix!r} must match" in r.stderr, prefix
    for name in OLD_HEADER_CASES:
        r = run(p, "--c", c, "--h", tmp_path / name, "--prefix", "sfx_")
        assert r.returncode == 64 and "hold sfx" in r.stderr, name
    assert sorted(f.name for f in tmp_path.iterdir()) == ["model.json"]


@pytest.mark.parametrize("prefix", ["sfx_", "uos_sfx_", "dx_sfx_", "a1_b2_sfx_",
                                    "a" * 27 + "_sfx_"])
def test_marker_prefixes_are_taken(prefix):
    assert T.PREFIX.match(prefix) and len(prefix) <= 32


def _header_names(count, seed):
    """count header names that H_FILE takes, made with a fixed seed. Short
    stems over few letters, so near names such as sfx_a.h and sfxa.h meet."""
    rng = random.Random(seed)
    out = []
    while len(out) < count:
        tail = "".join(rng.choice("a_1x") for _ in range(rng.randint(0, 3)))
        head = rng.choice(["", "a", "x_", "a1"])
        name = f"{head}sfx{tail}.h"
        if not name[0].isalpha():
            name = "s" + name
        assert T.H_FILE.fullmatch(name) and not T.file_name_faults("sfx.c", name), name
        out.append(name)
    return out


def test_header_guards_are_one_to_one():
    names = sorted(set(_header_names(400, 7)))
    assert len(names) > 100
    for i, a in enumerate(names):
        for b in names[i + 1:]:
            assert T.header_guard(a) != T.header_guard(b), (a, b)


def test_own_names_that_clash_are_refused():
    m = tone_model(split=4)
    beep = m["sounds"]["beep"]
    m["sounds"] = {"a": beep, "a_1": tone_model()["sounds"]["beep"]}
    built = {"a": bytes(8), "a_1": bytes(8)}
    faults = T.c_name_faults(T.c_names(m, built, "sfx_", "sfx_t.h"))
    assert faults and all("already given by" in f for f in faults)
    assert T.c_name_faults([("A", "one"), ("A", "two")]) == ["two give A, already given by one"]
    m["sounds"] = {"beep": tone_model()["sounds"]["beep"]}
    # The guard starts with SFX_GUARD_ and ends with the file's _H, and every
    # macro ends with its own suffix, so a guard never meets a macro, and
    # never a name C reserves such as E plus a capital.
    for h_name in ("sfx_beep_peak.h", "sfx_model_sha256.h", "esfx.h", "sigsfx.h"):
        names = T.c_names(m, {"beep": bytes(8)}, "sfx_", h_name)
        assert names[0][0].startswith("SFX_GUARD_") and names[0][0].endswith("_H")
        assert T.c_name_faults(names) == []
    assert T.header_guard("esfx.h") == "SFX_GUARD_ESFX_H"


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
    c, h = cdir / "sfx_t.c", hdir / "sfx_t.h"
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
    assert len(r.stderr.splitlines()) == 1
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
    c, h = cdir / "sfx_t.c", hdir / "sfx_t.h"
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
    # The renames, then the restore of the source after a failed header.
    assert [os.path.basename(x) for x in calls] == [
        ["sfx_table.c"], ["sfx_table.c", "sfx_table.h", "sfx_table.c"]][fail_at - 1]
    assert h.read_bytes() == old_h and c.read_bytes() == old_c
    _no_temp_left(tmp_path)
    assert not [f for f in os.listdir(tmp_path) if f.endswith(".bak")]
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
    assert not d.exists()           # the run made the folder, so it removes it


def test_generated_files_are_ascii_with_lf(tmp_path):
    _, c, h = _generate(tmp_path, _two_sounds())
    for f in (c, h):
        raw = f.read_bytes()
        raw.decode("ascii")
        assert b"\r" not in raw and raw.endswith(b"\n")


@pytest.mark.parametrize("which", ["c", "h"])
def test_crlf_copy_fails_the_full_check(tmp_path, which):
    p, c, h = _generate(tmp_path, _two_sounds())
    f = c if which == "c" else h
    f.write_bytes(f.read_bytes().replace(b"\n", b"\r\n"))
    args = ("--c", c, "--h", h, "--prefix", "demo_sfx_", "--check")
    assert run(p, *args).returncode == 0          # the fast check reads lines only
    r = run(p, *args, "--full")
    assert r.returncode == 1 and f"{f}: differs from a new render" in r.stderr


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
    assert "sound 'dive'.layers[0].partials[0]: the sine of multiple 1000000" in r.stderr


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
    ("demo_sfx_", "sfx_table.h"), ("sfx_", "t_sfx.h"), ("snd_sfx_", "sfx_x_table.h"),
    ("z9_sfx_", "my_sfx9.h"), ("q_sfx_", "h1sfx.h"), ("x_sfx_", "x_sfx_rate_hz.h"),
    ("size_sfx_", "big_sfx.h")])
def test_generated_c_compiles_for_names(tmp_path, prefix, h_name):
    m = _two_sounds()
    p = write_model(tmp_path, m)
    c, h = tmp_path / "out_sfx.c", tmp_path / h_name
    r = run(p, "--c", c, "--h", h, "--prefix", prefix)
    assert r.returncode == 0, r.stderr
    main = tmp_path / "main.c"
    main.write_text(f'#include "{h_name}"\n#include "{h_name}"\n'
                    f"int main(void)\n{{\n    return {prefix}beep[0] == 128 ? 0 : 1;\n}}\n")
    r = subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-I", str(tmp_path),
                        str(c), str(main), "-o", str(tmp_path / "a.out")],
                       capture_output=True, text=True, env=minimal_env())
    assert r.returncode == 0, r.stderr


# ---------------------------------------------------------------------------
# Every number a JSON integer, every field typed
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("value", ["1.0", "1e0", "1E0", "1.5e3", "-0.0", "10e-1"])
def test_format_must_be_a_json_integer(tmp_path, value):
    p = tmp_path / "m.json"
    p.write_text(json.dumps(tone_model()).replace('"format": 1', f'"format": {value}'))
    r = run(p, "--dump", tmp_path / "d")
    assert r.returncode == 2 and r.stderr.count("\n") == 1
    assert f"{value} is not a whole number" in r.stderr and "Traceback" not in r.stderr


@pytest.mark.parametrize("text,message", [
    ('{"format": Infinity}', "Infinity is not a number"),
    ('{"format": -Infinity}', "-Infinity is not a number"),
    ('{"format": 1, "rate_hz": 1' + "0" * 30 + "}", "too large for any field"),
    ('{"format": 1, "rate_hz": 8000.5}', "8000.5 is not a whole number"),
], ids=["inf", "-inf", "long", "decimal"])
def test_refusal_of_bad_numbers(tmp_path, text, message):
    p = tmp_path / "m.json"
    p.write_text(text)
    r = run(p, "--dump", tmp_path / "d")
    assert r.returncode == 2 and message in r.stderr and "Traceback" not in r.stderr


def test_refusal_of_deep_nesting(tmp_path):
    """Some Python versions parse 100000 nested arrays and some give up in
    the parser. Either way the tool refuses the model in one line."""
    p = tmp_path / "m.json"
    p.write_text("[" * 100000 + "]" * 100000)
    r = run(p, "--dump", tmp_path / "d")
    assert r.returncode == 2 and r.stderr.count("\n") == 1 and "Traceback" not in r.stderr
    assert "model: must be an object" in r.stderr or "not valid JSON" in r.stderr


def _spec_fields():
    text = open(os.path.join(ROOT, "docs", "spec", "SFX_MODEL.md"), encoding="utf-8").read()
    out = {}
    for title, section in (("Top level", "top"), ("A sound", "sound"),
                           ("A tone layer", "tone"), ("A noise layer", "noise")):
        body = text.split(f"### {title}\n", 1)[1].split("\n#", 1)[0]
        names = set()
        for row in body.splitlines():
            if row.startswith("| `"):
                names.update(re.findall(r"`([a-z_]+)`", row.split("|")[1]))
        out[section] = names
    return out


def test_field_list_matches_the_spec():
    assert _spec_fields() == {k: set(v) for k, v in T.FIELDS.items()}
    for k, v in T.REQUIRED.items():
        assert v <= set(T.FIELDS[k])


def _every_field():
    # A model that passes check_model and holds every field of the format.
    tone = {"kind": "tone", "hits": [[0, 0]], "len": 8, "pitch_mhz": [2000000, 2000000],
            "level_mdb": 0, "partials": [[10000, 0]], "attack_us": 100, "release_us": 100,
            "env": [[0, 0]]}
    noise = {"kind": "noise", "hits": [[0, 0]], "seed": 1, "level_mdb": 0,
             "env": [[0, 0], [1000, 0]], "bands": [[0, 0], [4000, 0]], "fold": 1,
             "repeat": False}
    sound = {"len": 8, "peak_steps": 8, "layers": [tone, noise], "loop": False, "split": 4}
    return {"format": 1, "rate_hz": 8000, "ceiling_steps": 90, "sounds": {"beep": sound}}


def _field_paths():
    m = _every_field()
    yield "top", (), m
    s = m["sounds"]["beep"]
    yield "sound", ("sounds", "beep"), s
    yield "tone", ("sounds", "beep", "layers", 0), s["layers"][0]
    yield "noise", ("sounds", "beep", "layers", 1), s["layers"][1]


def test_every_field_checks_its_type():
    # Each field of FIELDS, given a wrong type, gives a fault that names it.
    assert T.check_model(_every_field()) == []
    count = 0
    for section, path, obj in _field_paths():
        assert set(obj) == set(T.FIELDS[section])
        for key, kind in T.FIELDS[section].items():
            wrongs = {"int": [True, 1.0, 10 ** 30, "1", None], "bool": [0, 1, "true", None],
                      "kind": [1, True, "other"], "object": [[], 1, {}],
                      "list": [{}, 1, True], "pair": [[1.0, 1], [True, 1], 1],
                      "pairs": [[[1.0, 0]], [[0, True]], [[0, 10 ** 30]], [1], 1]}[kind]
            for wrong in wrongs:
                m = _every_field()
                target = m
                for k in path:
                    target = target[k]
                target[key] = wrong
                faults = T.check_model(m)
                assert any(f".{key}" in f or f.startswith(key) for f in faults), \
                    (section, key, wrong, faults)
                count += 1
    assert count > 80

# ---------------------------------------------------------------------------
# The sine bound per partial, and the backstop behind it
# ---------------------------------------------------------------------------

def _review_glide():
    # A falling glide where the highest multiple stays above half the rate,
    # while a lower one sounds near the end and passes the sine's limit.
    return {"format": 1, "rate_hz": 48000, "ceiling_steps": 127, "sounds": {"dive": {
        "len": 100000, "peak_steps": 100, "layers": [
            {"kind": "tone", "hits": [[0, 0]], "len": 100000,
             "pitch_mhz": [24000000, 1000000], "level_mdb": 0,
             "partials": [[200000, 0], [1000000, 0]]}]}}}


def test_every_partial_is_bounded(tmp_path):
    m = _review_glide()
    assert T.sine_bound(100000, 24000000, 1000000, 1000000, 48000) == 0.0
    assert T.sine_bound(100000, 24000000, 1000000, 200000, 48000) > T.SINE_LIMIT
    # Validation refuses it, not the backstop.
    faults = T.check_model(m)
    assert len(faults) == 1
    assert "sound 'dive'.layers[0].partials[0]: the sine of multiple 200000" in faults[0]
    r = run(write_model(tmp_path, m), "--dump", tmp_path / "d")
    assert r.returncode == 2 and r.stderr.count("\n") == 1
    assert "partials[0]: the sine of multiple 200000" in r.stderr
    assert T.BACKSTOP not in r.stderr and "Traceback" not in r.stderr


def test_sine_bound_holds_for_each_partial():
    # The largest argument the renderer passes to sin(), partial by partial,
    # against the bound. Glides both ways, and partials that start or stop
    # sounding part way.
    rng = random.Random(11)
    cases = [(3000, 24000000, 1000000, 200000, 48000), (3000, 1000000, 24000000, 90000, 48000),
             (2000, 4000000, 1000, 40000, 8000), (500, 1, 4000000, 1000000, 8000)]
    for _ in range(40):
        rate = rng.choice((8000, 22050, 48000))
        cases.append((rng.randint(1, 3000), rng.randint(1, rate * 500), rng.randint(1, rate * 500),
                      rng.randint(1, 1000000), rate))
    for n, a, b, mult, rate in cases:
        f0, f1 = a / 1000.0, b / 1000.0
        ratio = T.exp(T.ln(f1 / f0) / n)
        m = mult / 10000.0
        f, phase, worst = f0, 0.0, 0.0
        for _ in range(n):
            if f * m < rate / 2:
                worst = max(worst, phase * m)
            phase += T.TWO_PI * f / rate
            f *= ratio
        assert worst <= T.sine_bound(n, a, b, mult, rate), (n, a, b, mult, rate)


def test_backstop_names_the_sound_and_layer(tmp_path, monkeypatch, capsys):
    # Skip validation on purpose: the backstop then refuses the model.
    m = _review_glide()
    with pytest.raises(T.ModelError) as e:
        T.build(m)
    assert e.value.faults[0].startswith(f"sound 'dive': layers[0]: {T.BACKSTOP}: sin(")
    monkeypatch.setattr(T, "check_model", lambda model: [])
    assert T.main([str(write_model(tmp_path, m)), "--dump", str(tmp_path / "d")]) == 2
    err = capsys.readouterr().err
    assert err.count("\n") == 1 and T.BACKSTOP in err


def test_internal_error_is_one_line(tmp_path, monkeypatch, capsys):
    def boom(model):
        raise RuntimeError("broken\nhere")
    monkeypatch.setattr(T, "build", boom)
    p = write_model(tmp_path, tone_model())
    assert T.main([str(p), "--dump", str(tmp_path / "d")]) == 70
    err = capsys.readouterr().err
    assert err == f"sfx_table.py: {p}: internal error: RuntimeError: broken here\n"


def _fuzz_model(rng):
    """A random model that is valid by the schema: right types, ranges and
    shapes. The checks after rendering and the sine bound may refuse it."""
    rate = rng.choice((8000, 11025, 22050, 37500, 48000))

    def mdb():
        return rng.choice((MDB_LO, MDB_HI, 0, rng.randint(MDB_LO, MDB_HI), -rng.randint(0, 30000)))

    def pitch():
        return max(1, min(rate * 500, int(10 ** rng.uniform(0, math.log10(rate * 500)))))

    sounds = {}
    for k in range(rng.randint(1, 2)):
        slen = rng.randint(1, 400)
        layers = []
        for _ in range(rng.randint(1, 3)):
            hits = [[rng.randint(0, slen - 1), mdb()] for _ in range(rng.randint(1, 3))]
            if rng.random() < 0.08:
                # A long falling glide near the sine's limit, the review's
                # shape: a high silent partial and a lower one that sounds.
                n = rng.randint(20000, 120000)
                layer = {"kind": "tone", "hits": hits, "len": n,
                         "pitch_mhz": [rate * 500, int(10 ** rng.uniform(4, 6.5))],
                         "level_mdb": 0, "partials": [[int(10 ** rng.uniform(4.5, 6)), 0],
                                                      [1000000, 0]]}
            elif rng.random() < 0.75:
                n = rng.randint(1, 400)
                layer = {"kind": "tone", "hits": hits, "len": n, "pitch_mhz": [pitch(), pitch()],
                         "level_mdb": mdb(),
                         "partials": [[int(10 ** rng.uniform(0, 6)), mdb()]
                                      for _ in range(rng.randint(1, 4))]}
                us = n * 1000000 // rate
                if rng.random() < 0.5:
                    layer["attack_us"] = rng.randint(0, us)
                    layer["release_us"] = rng.randint(0, us)
                if rng.random() < 0.3:
                    t = sorted(rng.randint(0, us + 1000) for _ in range(rng.randint(1, 4)))
                    layer["env"] = [[x, mdb()] for x in t]
            else:
                t = sorted(rng.randint(0, 300 * 1000000 // rate) for _ in range(rng.randint(2, 4)))
                t[-1] = max(t[-1], 1)
                hz = sorted(rng.sample(range(0, rate // 2 + 1), rng.randint(2, 4)))
                layer = {"kind": "noise", "hits": hits, "seed": rng.randint(0, 0xFFFFFFFF),
                         "level_mdb": mdb(), "env": [[x, mdb()] for x in t],
                         "bands": [[f, mdb()] for f in hz]}
                n = T.ceil_div(t[-1] * rate, 1000000)
                if rng.random() < 0.5:
                    layer["fold"] = rng.randint(0, n // 2)
                if rng.random() < 0.3:
                    layer["repeat"] = rng.random() < 0.5
            layers.append(layer)
        sound = {"len": slen, "peak_steps": rng.randint(1, 127), "layers": layers}
        if rng.random() < 0.2:
            sound["loop"] = rng.random() < 0.5
        if slen > 1 and rng.random() < 0.2:
            sound["split"] = rng.randint(1, slen - 1)
        sounds[f"s{k}"] = sound
    return {"format": 1, "rate_hz": rate, "ceiling_steps": 127, "sounds": sounds}


MDB_LO, MDB_HI = -120000, 20000


def test_fuzz_never_reaches_the_backstop(tmp_path, capsys):
    # Every model renders, or validation or a check refuses it with exit 2.
    # None reaches the backstop, none is an internal error.
    rng = random.Random(2026)
    seen = {0: 0, 2: 0, "sine": 0}
    for i in range(300):
        m = _fuzz_model(rng)
        p = write_model(tmp_path, m, f"m{i}.json")
        code = T.main([str(p), "--dump", str(tmp_path / f"d{i}")])
        err = capsys.readouterr().err
        assert code in (0, 2), (m, err)
        assert T.BACKSTOP not in err and "internal error" not in err and "Traceback" not in err
        seen[code] += 1
        seen["sine"] += "the sine of multiple" in err
    # The fuzz reaches both outcomes and the sine bound.
    assert seen[0] > 20 and seen[2] > 20 and seen["sine"] > 5, seen

# ---------------------------------------------------------------------------
# File names
# ---------------------------------------------------------------------------

BAD_FILE_NAMES = [
    ("--h", 'x"q.h', "the file name must be"),
    ("--h", "söndsfx.h", "the file name must be"),
    ("--h", "x qsfx.h", "the file name must be"),
    ("--h", "x\\qsfx.h", "the file name must be"),
    ("--h", "x'qsfx.h", "the file name must be"),
    ("--h", "x*qsfx.h", "the file name must be"),
    ("--h", "1sfx.h", "the file name must be"),
    ("--h", "_xsfx.h", "the file name must be"),
    ("--h", "-xsfx.h", "the file name must be"),
    ("--h", ".sfx.h", "the file name must be"),
    ("--h", "sfx.hpp", "the file name must be"),
    ("--h", "sfx_rate_hz", "the file name must be"),
    ("--h", "sfx.h\n", "the file name must be"),
    ("--h", "stdint.h", "hold sfx"),
    ("--h", "SFX.H", "the file name must be"),
    ("--h", "Sfx_t.h", "hold sfx"),
    ("--h", "s_fx.h", "hold sfx"),
    ("--c", "out.c", "hold sfx"),
    ("--c", 'x"q.c', "the file name must be"),
    ("--c", "sfx.h", "the file name must be"),
    ("--c", "a bsfx.c", "the file name must be"),
    ("--c", "1sfx.c", "the file name must be"),
    ("--c", "söndsfx.c", "the file name must be"),
    ("--h", "sfx-a.h", "the file name must be"),
    ("--h", "my.sfx.h", "the file name must be"),
    ("--h", "Big_sfx.h", "the file name must be"),
    ("--c", "sfx_A.c", "the file name must be"),
    ("--c", "sfx-a.c", "the file name must be"),
]


@pytest.mark.parametrize("option,name,message", BAD_FILE_NAMES,
                         ids=[f"{o}-{n!r}" for o, n, _ in BAD_FILE_NAMES])
@pytest.mark.parametrize("check", [False, True])
def test_bad_file_names_are_refused(tmp_path, option, name, message, check):
    p = write_model(tmp_path, tone_model())
    names = {"--c": "sfx_t.c", "--h": "sfx_t.h", option: name}
    args = [p, "--c", tmp_path / names["--c"], "--h", tmp_path / names["--h"], "--prefix", "x_sfx_"]
    r = run(*args, *(["--check"] if check else []))
    assert r.returncode == 64, r.stderr
    errors = [line for line in r.stderr.splitlines() if "error:" in line]
    assert len(errors) == 1 and message in errors[0] and option in errors[0]
    assert "Traceback" not in r.stderr
    assert sorted(f.name for f in tmp_path.iterdir()) == ["model.json"]


def test_dump_and_wav_names_are_safe(tmp_path):
    m = _two_sounds()
    m["sounds"]["a"] = tone_model(split=4)["sounds"]["beep"]
    m["sounds"]["z9_"] = tone_model()["sounds"]["beep"]
    p = write_model(tmp_path, m)
    r = run(p, "--dump", tmp_path / "d", "--wav", tmp_path / "w")
    assert r.returncode == 0, r.stderr
    names = os.listdir(tmp_path / "d") + os.listdir(tmp_path / "w")
    assert "a_1.u8" in names and "z9_.wav" in names
    for n in names:
        assert re.fullmatch(r"[a-z][a-z0-9_]*\.(u8|wav)", n), n


# ---------------------------------------------------------------------------
# The plan: every file a run reads and writes, checked whole first
# ---------------------------------------------------------------------------

def _alias(tmp_path, how):
    """(args, files the run must leave as they are) for one spelling of
    the same file twice."""
    p = write_model(tmp_path, tone_model(), name="sfx.c")
    sub = tmp_path / "sub"
    sub.mkdir()
    c, h = tmp_path / "sfx_t.c", tmp_path / "sfx_t.h"
    if how == "model as --c":
        return [p, "--c", p, "--h", h, "--prefix", "x_sfx_"]
    if how == "relative and absolute":
        return [p, "--c", os.path.relpath(p), "--h", h, "--prefix", "x_sfx_"]
    if how == "dot and dot-dot":
        return [p, "--c", sub / ".." / "." / "sfx.c", "--h", h, "--prefix", "x_sfx_"]
    if how == "symbolic link":
        (sub / "sfx_link.c").symlink_to(p)
        return [p, "--c", sub / "sfx_link.c", "--h", h, "--prefix", "x_sfx_"]
    if how == "hard link":
        try:
            os.link(p, sub / "sfx_hard.c")
        except OSError as e:
            pytest.skip(f"this file system makes no hard link: {e}")
        return [p, "--c", sub / "sfx_hard.c", "--h", h, "--prefix", "x_sfx_"]
    if how == "case":
        q = tmp_path / "Sfx.c"
        if not q.exists():
            q.write_bytes(p.read_bytes())
        return [q, "--c", p, "--h", h, "--prefix", "x_sfx_"]
    if how == "header link to source":
        (sub / "x_sfx_t.h").symlink_to(c)
        c.write_text("old\n")
        return [p, "--c", c, "--h", sub / "x_sfx_t.h", "--prefix", "x_sfx_"]
    if how == "--c is a directory":
        (sub / "sfx_d.c").mkdir()
        return [p, "--c", sub / "sfx_d.c", "--h", h, "--prefix", "x_sfx_"]
    if how == "missing folder":
        return [p, "--c", tmp_path / "none" / "sfx_t.c", "--h", h, "--prefix", "x_sfx_"]
    if how == "--dump is a file":
        (tmp_path / "f").write_text("x")
        return [p, "--dump", tmp_path / "f"]
    if how == "--dump in a missing folder":
        return [p, "--dump", tmp_path / "none" / "d"]
    if how == "--wav file is the model":
        q = write_model(sub, tone_model(), name="beep.wav")
        return [q, "--wav", sub]
    raise AssertionError(how)


ALIASES = ["model as --c", "relative and absolute", "dot and dot-dot", "symbolic link",
           "hard link", "case", "header link to source", "--c is a directory",
           "missing folder", "--dump is a file", "--dump in a missing folder",
           "--wav file is the model"]


def _run_in(cwd, *args):
    return subprocess.run([sys.executable, "-I", TOOL, *map(str, args)], capture_output=True,
                          text=True, env=minimal_env(), cwd=cwd)


@pytest.mark.parametrize("how", ["missing middle", "link in the middle"])
def test_a_path_is_taken_by_where_it_resolves(tmp_path, how):
    """The plan resolves each path once and the run writes only there, so
    a spelling that cannot be walked still works, and --check of the same
    command line reads the same files."""
    write_model(tmp_path, tone_model())
    if how == "missing middle":
        out, where = "missing/../out", tmp_path / "out"
    else:
        (tmp_path / "a" / "real").mkdir(parents=True)
        (tmp_path / "link").symlink_to(tmp_path / "a" / "real")
        out, where = "link/../out", tmp_path / "a" / "out"
    table = ["model.json", "--c", f"{out}/sfx_t.c", "--h", f"{out}/sfx_t.h",
             "--prefix", "x_sfx_"]
    r = _run_in(tmp_path, *table, "--dump", out)
    assert r.returncode == 0, r.stderr
    assert sorted(f.name for f in where.iterdir()) == ["beep.u8", "sfx_t.c", "sfx_t.h"]
    assert not (tmp_path / "missing").exists()
    if how != "missing middle":
        assert not (tmp_path / "out").exists()
    for extra in ([], ["--full"]):
        r = _run_in(tmp_path, *table, "--check", *extra)
        assert r.returncode == 0, r.stderr


@pytest.mark.parametrize("how", ALIASES)
def test_plan_refuses_the_same_file_twice(tmp_path, how, monkeypatch):
    monkeypatch.chdir(tmp_path)
    args = _alias(tmp_path, how)
    before = {f: f.read_bytes() for f in tmp_path.rglob("*") if f.is_file()}
    r = subprocess.run([sys.executable, "-I", TOOL, *map(str, args)], capture_output=True,
                       text=True, env=minimal_env(), cwd=tmp_path)
    assert r.returncode == 64, r.stderr
    errors = [x for x in r.stderr.splitlines() if "error:" in x]
    assert errors and "Traceback" not in r.stderr
    after = {f: f.read_bytes() for f in tmp_path.rglob("*") if f.is_file()}
    assert after == before


def _nodes_case(tmp_path, how):
    """(args, words the one error line holds) for a plan whose nodes clash."""
    p = write_model(tmp_path, tone_model())
    c, h = tmp_path / "sfx_t.c", tmp_path / "sfx_t.h"
    if how == "--c and --dump at one path":
        o = tmp_path / "out_sfx.c"
        return [p, "--c", o, "--h", h, "--prefix", "sfx_", "--dump", o], "a file and a folder"
    if how in ("--dump under --c", "--dump under --c, --dump first"):
        (tmp_path / "d").mkdir()
        c = tmp_path / "d" / "sfx.c"
        dump = ["--dump", c / "x"]
        table = ["--c", c, "--h", h, "--prefix", "sfx_"]
        args = dump + table if how.endswith("first") else table + dump
        return [p, *args], "lies under"
    if how == "--wav at the header":
        return [p, "--c", c, "--h", h, "--prefix", "sfx_", "--wav", h], "a file and a folder"
    if how == "new folder is --c by a link":
        (tmp_path / "sfx_l.c").symlink_to(tmp_path / "new")
        return ([p, "--c", tmp_path / "sfx_l.c", "--h", h, "--prefix", "sfx_",
                 "--dump", tmp_path / "new"], "a file and a folder")
    if how == "new folder is --c by case":
        return ([p, "--c", tmp_path / "out_sfx.c", "--h", h, "--prefix", "sfx_",
                 "--wav", tmp_path / "OUT_sfx.c"], "a file and a folder")
    if how == "the model's folder as --c":
        sub = tmp_path / "m_sfx.c"
        sub.mkdir()
        q = write_model(sub, tone_model())
        return [q, "--c", sub, "--h", h, "--prefix", "sfx_"], "a file and a folder"
    raise AssertionError(how)


NODE_CASES = ["--c and --dump at one path", "--dump under --c", "--dump under --c, --dump first",
              "--wav at the header", "new folder is --c by a link", "new folder is --c by case",
              "the model's folder as --c"]


@pytest.mark.parametrize("how", NODE_CASES)
def test_plan_refuses_a_path_that_is_a_file_and_a_folder(tmp_path, how):
    args, words = _nodes_case(tmp_path, how)
    names = lambda: sorted(str(x.relative_to(tmp_path)) for x in tmp_path.rglob("*"))
    before = names()
    r = run(*args)
    assert r.returncode == 64, r.stderr
    errors = [x for x in r.stderr.splitlines() if "error:" in x]
    assert len(errors) == 1 and words in errors[0], r.stderr
    assert "Traceback" not in r.stderr and names() == before


def test_dump_and_wav_may_share_a_folder(tmp_path):
    p = write_model(tmp_path, _two_sounds())
    r = run(p, "--dump", tmp_path / "o", "--wav", tmp_path / "o")
    assert r.returncode == 0, r.stderr
    assert {"beep.u8", "beep.wav", "hiss.wav"} <= set(os.listdir(tmp_path / "o"))


# ---------------------------------------------------------------------------
# One publish for the whole run
# ---------------------------------------------------------------------------

def _combined(tmp_path):
    m = _two_sounds()
    p = write_model(tmp_path, m)
    c, h = tmp_path / "x_sfx_t.c", tmp_path / "x_sfx_t.h"
    d, w = tmp_path / "d", tmp_path / "w"
    args = [str(p), "--c", str(c), "--h", str(h), "--prefix", "x_sfx_",
            "--dump", str(d), "--wav", str(w)]
    assert T.main(args) == 0
    m["sounds"]["beep"]["len"] = 10
    write_model(tmp_path, m)
    return args, [c, h, *sorted(d.iterdir()), *sorted(w.iterdir())]


def _snapshot(files):
    return {f: f.read_bytes() for f in files}


# The order of the renames: OUT.c, each .u8, each .wav, OUT.h.
@pytest.mark.parametrize("fail_at", ["temp", "backup", "rename 1", "rename dump",
                                     "rename wav", "rename header"])
def test_combined_run_rolls_back(tmp_path, monkeypatch, capsys, fail_at):
    args, files = _combined(tmp_path)
    old = _snapshot(files)
    real_replace, real_open, real_link = os.replace, os.open, os.link
    renames = []
    stop = {"rename 1": 1, "rename dump": 2, "rename wav": len(files) - 1,
            "rename header": len(files)}.get(fail_at)

    def replace(a, b):
        if not str(a).endswith(".bak"):
            renames.append(b)
            if len(renames) == stop:
                raise OSError(5, "Input/output error", str(b))
        real_replace(a, b)

    def opener(path, *a, **k):
        if fail_at == "temp" and str(path).endswith(".wav.%d.tmp" % os.getpid()):
            raise OSError(28, "No space left on device", str(path))
        return real_open(path, *a, **k)

    def link(a, b, **k):
        if fail_at == "backup" and str(b).endswith(".wav.%d.bak" % os.getpid()):
            raise OSError(1, "Operation not permitted", str(b))
        return real_link(a, b, **k)

    monkeypatch.setattr(T.os, "replace", replace)
    monkeypatch.setattr(T.os, "open", opener)
    monkeypatch.setattr(T.os, "link", link)
    code = T.main(args)
    monkeypatch.setattr(T.os, "replace", real_replace)
    monkeypatch.setattr(T.os, "open", real_open)
    monkeypatch.setattr(T.os, "link", real_link)
    if fail_at == "backup":
        assert code == 0      # a backup falls back to a copy
        return
    assert code == 1
    assert _snapshot(files) == old, "every old file is back"
    for folder in (tmp_path, tmp_path / "d", tmp_path / "w"):
        assert not [f for f in os.listdir(folder) if f.endswith((".tmp", ".bak"))]
    capsys.readouterr()
    assert T.main(args[:7] + ["--check"]) == 1
    assert "the model changed" in capsys.readouterr().err


def test_combined_run_removes_new_files_and_folders(tmp_path, monkeypatch):
    m = _two_sounds()
    p = write_model(tmp_path, m)
    args = [str(p), "--c", str(tmp_path / "x_sfx_t.c"), "--h", str(tmp_path / "x_sfx_t.h"),
            "--prefix", "x_sfx_", "--dump", str(tmp_path / "d"), "--wav", str(tmp_path / "w")]
    real = os.replace
    n = []

    def replace(a, b):
        n.append(b)
        if str(b).endswith(".h"):
            raise OSError(5, "Input/output error", str(b))
        real(a, b)

    monkeypatch.setattr(T.os, "replace", replace)
    assert T.main(args) == 1
    assert sorted(os.listdir(tmp_path)) == ["model.json"]


def test_finding_dump_rolled_back_when_wav_fails(tmp_path, monkeypatch):
    m = _two_sounds()
    p = write_model(tmp_path, m)
    d, w = tmp_path / "d", tmp_path / "w"
    real = os.replace

    def replace(a, b):
        if str(b).endswith(".wav"):
            raise OSError(5, "Input/output error", str(b))
        real(a, b)

    monkeypatch.setattr(T.os, "replace", replace)
    assert T.main([str(p), "--dump", str(d), "--wav", str(w)]) == 1
    assert not d.exists() and not w.exists()


def test_finding_model_named_like_the_source(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    p = write_model(tmp_path, tone_model(), name="sfx.c")
    old = p.read_bytes()
    r = subprocess.run([sys.executable, "-I", TOOL, "sfx.c", "--c", "sfx.c", "--h",
                        "x_sfx_t.h", "--prefix", "x_sfx_"], capture_output=True, text=True,
                       env=minimal_env(), cwd=tmp_path)
    assert r.returncode == 64 and "MODEL 'sfx.c' and --c 'sfx.c'" in r.stderr
    assert p.read_bytes() == old and sorted(os.listdir(tmp_path)) == ["sfx.c"]


# ---------------------------------------------------------------------------
# A one-shot noise hit filters only the samples that sound
# ---------------------------------------------------------------------------

def _old_render_noise(layer, sound, out, rate, where, direct=False):
    """render_noise() as it was before the cut: every hit filtered whole."""
    loop = sound.get("loop", False)
    h = T.fir_design(layer["bands"], rate)
    gain = T.noise_gain(layer, rate)
    n = len(gain)
    same = None
    conv = T.convolve_direct if direct else T.convolve
    for k, (at, hit_mdb) in enumerate(layer["hits"]):
        if layer.get("repeat") and same is not None:
            one = same
        else:
            w = T.white(T.start_state(layer["seed"], k), n + T.FIR_TAPS - 1)
            y = conv(h, w, n)
            one = [y[i] * gain[i] if gain[i] else 0.0 for i in range(n)]
            same = one
        m = n if loop else min(n, len(out) - at)
        T._add(out, at, one[:m], T.db_gain((layer["level_mdb"] + hit_mdb) / 1000.0), loop)


def _cut_model(hits, repeat=False, fold=0, loop=False):
    # n = 800 samples at 20000 Hz.
    layer = {"kind": "noise", "seed": 3, "level_mdb": 0, "hits": hits,
             "env": [[0, -20000], [5000, 0], [40000, -30000]],
             "bands": [[300, -9000], [2000, 0], [8000, -20000]]}
    if fold:
        layer["fold"] = fold
    if repeat:
        layer["repeat"] = True
    sound = {"len": 1000, "peak_steps": 20, "layers": [layer]}
    if loop:
        sound["loop"] = True
    return {"format": 1, "rate_hz": 20000, "ceiling_steps": 90, "sounds": {"s": sound}}


# Hits that run past the end by 1, by half and by all but 1 of their 800.
CUT_HITS = [[[201, 0]], [[600, -3000]], [[999, 0]], [[0, 0], [201, -2000], [600, 0], [999, -6000]],
            [[999, 0], [600, -1000], [0, 0]]]


@pytest.mark.parametrize("hits", CUT_HITS)
@pytest.mark.parametrize("repeat,fold,loop", [(False, 0, False), (True, 0, False),
                                              (False, 300, False), (True, 300, False),
                                              (False, 0, True)])
def test_cut_hit_is_bit_equal_to_the_whole_render(monkeypatch, hits, repeat, fold, loop):
    m = _cut_model(hits, repeat, fold, loop)
    new = T.render_float(m, "s")
    monkeypatch.setattr(T, "render_noise", _old_render_noise)
    old = T.render_float(m, "s")
    assert [struct.pack("<d", v) for v in new] == [struct.pack("<d", v) for v in old]


def test_cut_hit_filters_only_what_sounds(monkeypatch):
    m = _cut_model([[999, 0]] * 3)
    lengths = []
    real = T.convolve

    def conv(h, w, n):
        lengths.append(n)
        return real(h, w, n)

    monkeypatch.setattr(T, "convolve", conv)
    T.render_float(m, "s")
    assert lengths == [1, 1, 1]


def test_late_hits_on_a_long_noise_are_cheap():
    # The review's case: a 262144-sample hit, 64 hits on the last sample.
    layer = {"kind": "noise", "seed": 1, "level_mdb": 0, "hits": [[999, 0]] * 64,
             "env": [[0, 0], [5461333, 0]], "bands": [[0, 0], [8000, 0]]}
    m = {"format": 1, "rate_hz": 48000, "ceiling_steps": 90,
         "sounds": {"s": {"len": 1000, "peak_steps": 20, "layers": [layer]}}}
    assert T.noise_len(layer, 48000) == 262144
    assert T.check_model(m) == []
    assert T.render_cost(m) < 10 ** 7
    T.render_float(m, "s")


# ---------------------------------------------------------------------------
# The render budget
# ---------------------------------------------------------------------------

def _noise_loop(seconds, layers, tone=False, rate=48000):
    n = round(seconds * rate)
    out = [{"kind": "noise", "seed": s, "level_mdb": -3000, "hits": [[0, 0]],
            "env": [[0, 0], [round(seconds * 1000000), 0]],
            "bands": [[0, -6000], [200, 0], [1500, -6000], [6000, -30000]]}
           for s in range(layers)]
    if tone:
        out.append({"kind": "tone", "hits": [[0, 0]], "len": n, "pitch_mhz": [100000, 100000],
                    "level_mdb": 0, "partials": [[10000 * (k + 1), 0] for k in range(16)]})
    return {"format": 1, "rate_hz": rate, "ceiling_steps": 60, "sounds": {"l": {
        "len": n, "peak_steps": 20, "loop": True, "layers": out}}}


def test_a_three_second_loop_is_in_the_budget():
    m = _noise_loop(3, 2, tone=True)
    assert T.check_model(m) == []
    assert T.render_cost(m) < T.MAX_COST // 4


def test_a_model_over_the_budget_is_refused_in_one_line(tmp_path):
    m = _noise_loop(5, 9)
    assert T.render_cost(m) > T.MAX_COST
    r = run(write_model(tmp_path, m), "--dump", tmp_path / "d")
    assert r.returncode == 2
    lines = r.stderr.splitlines()
    assert len(lines) == 1 and "over the budget of 1000000000" in lines[0], r.stderr
    assert not (tmp_path / "d").exists()


def test_budget_counts_kept_samples_only():
    m = _cut_model([[999, 0]] * 4)
    assert T.render_cost(m) == 1000 * T.SINE_COST + 4 * T.FIR_TAPS + T.DESIGN_COST
    m = _cut_model([[999, 0]] * 4, repeat=True)
    assert T.render_cost(m) == 1000 * T.SINE_COST + T.FIR_TAPS + T.DESIGN_COST
    m = _cut_model([[999, 0]] * 4, loop=True)
    assert T.render_cost(m) == 1000 * T.SINE_COST + 4 * 800 * T.FIR_TAPS + T.DESIGN_COST


# ---------------------------------------------------------------------------
# A failure at any file operation of a publish leaves the folder as it was
# ---------------------------------------------------------------------------

def _tree(root):
    out = {}
    for folder, dirs, files in os.walk(root):
        for d in dirs:
            out[os.path.relpath(os.path.join(folder, d), root)] = "dir"
        for f in files:
            p = os.path.join(folder, f)
            with open(p, "rb") as fh:
                out[os.path.relpath(p, root)] = fh.read()
    return out


class _Writer:
    """A file whose write() goes through a counted fake."""

    def __init__(self, f, hook):
        self.f, self.hook = f, hook

    def write(self, data):
        self.hook("write")
        return self.f.write(data)

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.f.close()


def _faulty(monkeypatch, fail_op, fail_n, no_link):
    """Count every file operation of publish; raise on the fail_n-th call of
    fail_op. With no_link every hard link fails, so backups are copies."""
    counts = {}
    real = {k: getattr(os, k) for k in ("mkdir", "open", "fdopen", "link", "replace")}
    real_open = open

    def hook(op):
        counts[op] = counts.get(op, 0) + 1
        if op == fail_op and counts[op] == fail_n:
            raise OSError(28, "No space left on device", op)

    def wrap(name):
        def f(*a, **k):
            hook(name)
            if name == "link" and no_link:
                raise OSError(1, "Operation not permitted", a[1])
            r = real[name](*a, **k)
            return _Writer(r, hook) if name == "fdopen" else r
        return f

    for name in real:
        monkeypatch.setattr(T.os, name, wrap(name))

    def reader(*a, **k):
        hook("read open")
        return real_open(*a, **k)

    monkeypatch.setattr(T, "open", reader, raising=False)
    return counts


def _publish_run(tmp_path, fresh):
    m = _two_sounds()
    p = write_model(tmp_path, m)
    args = [str(p), "--c", str(tmp_path / "x_sfx_t.c"), "--h", str(tmp_path / "x_sfx_t.h"),
            "--prefix", "x_sfx_", "--dump", str(tmp_path / "w" / "d"), "--wav", str(tmp_path / "w")]
    if not fresh:
        assert T.main(args) == 0
        m["sounds"]["beep"]["len"] = 10
        write_model(tmp_path, m)
    return args


def _op_counts(tmp_path, fresh, no_link, monkeypatch):
    base = tmp_path / "count"
    base.mkdir()
    args = _publish_run(base, fresh)
    with monkeypatch.context() as mp:
        counts = _faulty(mp, None, 0, no_link)
        assert T.main(args) == 0
    shutil.rmtree(base)
    return counts


@pytest.mark.parametrize("fresh", [True, False])
@pytest.mark.parametrize("no_link", [False, True])
def test_a_failure_at_each_file_operation_leaves_the_folder(tmp_path, monkeypatch, capsys,
                                                            fresh, no_link):
    counts = _op_counts(tmp_path, fresh, no_link, monkeypatch)
    if not fresh:
        assert counts.get("replace", 0) > 0 and counts.get("link", 0) > 0
        if no_link:
            assert counts.get("read open", 0) > 0
    else:
        assert counts.get("mkdir") == 2
    tried = 0
    for op, total in sorted(counts.items()):
        for n in range(1, total + 1):
            root = tmp_path / f"{op.replace(' ', '_')}_{n}"
            root.mkdir()
            args = _publish_run(root, fresh)
            before = _tree(root)
            with monkeypatch.context() as mp:
                _faulty(mp, op, n, no_link)
                code = T.main(args)
            if op == "link":
                assert code == 0, "a failed link falls back to a copy"
                assert not [f for f in _tree(root) if f.endswith((".tmp", ".bak"))]
            else:
                assert code == 1, (op, n)
                assert _tree(root) == before, (op, n)
            tried += 1
    assert tried == sum(counts.values())
    capsys.readouterr()


def test_failed_backup_copy_leaves_no_partial_backup(tmp_path, monkeypatch, capsys):
    # The review's case: the link fails, then the copy's read fails.
    args = _publish_run(tmp_path, fresh=False)
    before = _tree(tmp_path)
    with monkeypatch.context() as mp:
        _faulty(mp, "read open", 1, no_link=True)
        assert T.main(args) == 1
    assert _tree(tmp_path) == before
    capsys.readouterr()


# ---------------------------------------------------------------------------
# New --dump and --wav folders: one folder under two spellings
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("dump,wav", [("out", "./out"), ("out/", "out"),
                                      ("lnk/out", "out"), ("out/../out", "out")])
def test_two_spellings_of_one_new_folder(tmp_path, monkeypatch, dump, wav):
    monkeypatch.chdir(tmp_path)
    os.symlink(".", "lnk")
    p = write_model(tmp_path, _two_sounds())
    r = subprocess.run([sys.executable, "-I", TOOL, str(p), "--dump", dump, "--wav", wav],
                       capture_output=True, text=True, env=minimal_env(), cwd=tmp_path)
    assert r.returncode == 0, r.stderr
    assert {"beep.u8", "beep.wav", "hiss.wav"} <= set(os.listdir(tmp_path / "out"))


def test_new_folders_that_differ_by_case_are_refused(tmp_path):
    p = write_model(tmp_path, _two_sounds())
    r = run(p, "--dump", tmp_path / "out", "--wav", tmp_path / "OUT")
    assert r.returncode == 64 and "differ only by case" in r.stderr
    assert sorted(os.listdir(tmp_path)) == ["model.json"]


def test_new_dump_folder_inside_a_new_wav_folder(tmp_path):
    p = write_model(tmp_path, _two_sounds())
    r = run(p, "--dump", tmp_path / "w" / "d", "--wav", tmp_path / "w")
    assert r.returncode == 0, r.stderr
    assert "beep.u8" in os.listdir(tmp_path / "w" / "d")
    assert "beep.wav" in os.listdir(tmp_path / "w")


def _race(monkeypatch):
    """Another process makes each new folder just before the run does."""
    real = os.mkdir

    def mkdir(path, *a, **k):
        real(path, *a, **k)
        raise FileExistsError(17, "File exists", path)

    monkeypatch.setattr(T.os, "mkdir", mkdir)


def test_a_folder_made_by_another_process_is_used(tmp_path, monkeypatch):
    p = write_model(tmp_path, _two_sounds())
    _race(monkeypatch)
    assert T.main([str(p), "--dump", str(tmp_path / "d"), "--wav", str(tmp_path / "w")]) == 0
    assert "beep.u8" in os.listdir(tmp_path / "d")


def test_roll_back_keeps_a_folder_it_did_not_make(tmp_path, monkeypatch, capsys):
    p = write_model(tmp_path, _two_sounds())
    _race(monkeypatch)
    real = os.replace

    def replace(a, b):
        if str(b).endswith(".wav"):
            raise OSError(5, "Input/output error", str(b))
        real(a, b)

    monkeypatch.setattr(T.os, "replace", replace)
    assert T.main([str(p), "--dump", str(tmp_path / "d"), "--wav", str(tmp_path / "w")]) == 1
    assert os.listdir(tmp_path / "d") == [] and os.listdir(tmp_path / "w") == []
    capsys.readouterr()


# ---------------------------------------------------------------------------
# A path is resolved once, in the plan
# ---------------------------------------------------------------------------

# Path calls allowed outside class Plan, each with its reason in the tool:
# _side and _backup join a side file's name to a resolved path.
PATH_CALLS_OUTSIDE_PLAN = {"join": 2}


def test_paths_resolve_only_in_the_plan():
    import ast
    tree = ast.parse(open(TOOL, encoding="utf-8").read())
    plan = next(n for n in tree.body if isinstance(n, ast.ClassDef) and n.name == "Plan")
    inside = {id(n) for n in ast.walk(plan)}
    found = {}
    for n in ast.walk(tree):
        name = n.attr if isinstance(n, ast.Attribute) else n.id if isinstance(n, ast.Name) else None
        if name in ("realpath", "abspath", "join") and id(n) not in inside:
            if name == "join" and not (isinstance(n, ast.Attribute)
                                       and isinstance(n.value, ast.Attribute)
                                       and n.value.attr == "path"):
                continue        # str.join, not a path join
            found[name] = found.get(name, 0) + 1
    assert found == PATH_CALLS_OUTSIDE_PLAN


def test_publish_writes_where_the_plan_resolved(tmp_path, monkeypatch, capsys):
    # Another program retargets the output's link to the model right after
    # the plan. The run writes where the plan resolved; the model stays.
    model = write_model(tmp_path, tone_model())
    before = model.read_bytes()
    target = tmp_path / "target_sfx.c"
    target.write_bytes(b"old")
    link = tmp_path / "sfx_t.c"
    link.symlink_to(target)
    plan_fault = T.Plan.fault

    def fault(self):
        r = plan_fault(self)
        link.unlink()
        link.symlink_to(model)
        return r
    monkeypatch.setattr(T.Plan, "fault", fault)
    code = T.main([str(model), "--c", str(link), "--h", str(tmp_path / "sfx_t.h"),
                   "--prefix", "x_sfx_"])
    assert code == 0, capsys.readouterr().err
    assert model.read_bytes() == before
    assert target.read_bytes().startswith(b"/*")


# ---------------------------------------------------------------------------
# Every independent field gets its line
# ---------------------------------------------------------------------------

def _broken(edit):
    m = _every_field()
    edit(m, m["sounds"]["beep"], *m["sounds"]["beep"]["layers"])
    return T.check_model(m)


S, L0, L1 = "sound 'beep'", "sound 'beep'.layers[0]", "sound 'beep'.layers[1]"


def _cut_top(m, s, tone, noise):
    del m["ceiling_steps"]
    m["extra"] = 1
    s["peak_steps"] = 0


def _cut_sound(m, s, tone, noise):
    del s["len"]
    s["peak_steps"] = "x"
    s["loop"] = 1


def _cut_tone(m, s, tone, noise):
    del tone["pitch_mhz"]
    tone["level_mdb"] = "x"
    tone["attack_us"] = -1


def _cut_noise(m, s, tone, noise):
    del noise["seed"]
    noise["repeat"] = "yes"
    noise["fold"] = -1


def _cut_items(m, s, tone, noise):
    del tone["len"]
    tone["hits"] = [[0, 0], [0, "x"]]
    tone["partials"] = [[0, 0]]


def _cut_short_list(m, s, tone, noise):
    del noise["seed"]
    noise["bands"] = [[-1, 0]]


LEVELS = [
    (_cut_top, ["extra: unknown field", "model: missing field 'ceiling_steps'",
            f"{S}.peak_steps: must be 1 to 127, not 0"]),
    (_cut_sound, [f"{S}: missing field 'len'", f"{S}.peak_steps: must be a whole number",
              f"{S}.loop: must be true or false"]),
    (_cut_tone, [f"{L0}: missing field 'pitch_mhz'", f"{L0}.level_mdb: must be a whole number",
             f"{L0}.attack_us: must be 0 to 32768000, not -1"]),
    (_cut_noise, [f"{L1}: missing field 'seed'", f"{L1}.repeat: must be true or false",
              f"{L1}.fold: must be 0 to 4, not -1"]),
    (_cut_items, [f"{L0}: missing field 'len'", f"{L0}.hits[1][1]: must be a whole number",
              f"{L0}.partials[0][0]: must be 1 to 1000000, not 0"]),
    (_cut_short_list, [f"{L1}: missing field 'seed'", f"{L1}.bands: must hold 2 to 24 points, not 1",
                   f"{L1}.bands[0][0]: must be 0 to 4000, not -1"]),
]


@pytest.mark.parametrize("edit,lines", LEVELS, ids=[e.__name__ for e, _ in LEVELS])
def test_a_missing_field_hides_no_other_fault(edit, lines):
    assert _broken(edit) == lines
    assert _broken(edit) == lines           # a stable order


def _over_budget():
    tone = {"kind": "tone", "hits": [[0, 0]], "len": T.MAX_LEN, "pitch_mhz": [1000, 1000],
            "level_mdb": 0, "partials": [[10000, 0]] * T.MAX_PARTIALS}
    return {"format": 1, "rate_hz": 8000, "ceiling_steps": 127, "sounds": {"big": {
        "len": T.MAX_LEN, "peak_steps": 8, "layers": [dict(tone) for _ in range(T.MAX_LAYERS)]}}}


def _split_past_len():
    m = tone_model(split=20)
    assert any(".split" in f for f in T.check_model(m))
    return m


def _attack_past_len():
    m = tone_model()
    m["sounds"]["beep"]["layers"][0]["attack_us"] = 5000
    assert any("must fit inside" in f for f in T.check_model(m))
    return m


def _fold_past_env():
    m = noise_model()
    m["sounds"]["hiss"]["layers"][0]["fold"] = 1000
    return m


# (model, path to the object, inputs, words of the dependent fault)
DEPENDENT = [
    (_review_glide, ("sounds", "dive", "layers", 0), ("len", "pitch_mhz", "partials"),
     "the sine of multiple"),
    (_review_glide, (), ("rate_hz",), "the sine of multiple"),
    (_over_budget, ("sounds", "big"), ("len",), "budget"),
    (_over_budget, ("sounds", "big", "layers", 0), ("len", "partials"), "budget"),
    (_split_past_len, ("sounds", "beep"), ("len",), ".split"),
    (_attack_past_len, ("sounds", "beep", "layers", 0), ("len",), "must fit inside"),
    (_attack_past_len, (), ("rate_hz",), "must fit inside"),
    (_fold_past_env, ("sounds", "hiss", "layers", 0), ("env",), ".fold"),
    (_fold_past_env, (), ("rate_hz",), ".fold"),
]
WRONG = [None, "x", True, [], {}, 1.5]


@pytest.mark.parametrize("make,path,keys,words", DEPENDENT)
def test_a_dependent_check_waits_for_its_inputs(make, path, keys, words):
    assert any(words in f for f in T.check_model(make()))
    for key in keys:
        for wrong in ["delete"] + WRONG:
            m = make()
            obj = m
            for k in path:
                obj = obj[k]
            if wrong == "delete":
                del obj[key]
            else:
                obj[key] = wrong
            faults = T.check_model(m)
            assert not any(words in f for f in faults), (key, wrong, faults)
            assert any(f"missing field {key!r}" in f or f".{key}" in f or f.startswith(key)
                       for f in faults), (key, wrong, faults)


def _damage(rng, m):
    """Damage one to three leaf fields of model m. Returns (where, key) of
    each: a line must name it."""
    spots = [("", m, "top", k) for k in ("rate_hz", "ceiling_steps")]
    for name, s in m["sounds"].items():
        w = f"sound {name!r}"
        spots += [(w, s, "sound", k) for k in s if k != "layers"]
        for li, layer in enumerate(s["layers"]):
            spots += [(f"{w}.layers[{li}]", layer, layer["kind"], k)
                      for k in layer if k != "kind"]
    done = []
    for where, obj, section, key in rng.sample(spots, rng.randint(1, min(3, len(spots)))):
        kind = T.FIELDS[section][key]
        how = rng.choice(("delete", "type", "range"))
        if how == "delete" and key in T.REQUIRED[section]:
            del obj[key]
        elif how == "range" and kind in ("int", "pair", "pairs"):
            obj[key] = {"int": 10 ** 12, "pair": [10 ** 12, 1],
                        "pairs": [[10 ** 12, 0], [10 ** 12, 0]]}[kind]
        else:
            obj[key] = "x"
        done.append((where, key))
    return done


def test_fuzz_of_broken_models(tmp_path, capsys):
    # Each damaged field gets its line, and no run reaches a traceback or
    # the internal error line.
    rng = random.Random(2027)
    for i in range(200):
        m = _fuzz_model(rng)
        damaged = _damage(rng, m)
        p = write_model(tmp_path, m, f"m{i}.json")
        code = T.main([str(p), "--dump", str(tmp_path / f"d{i}")])
        err = capsys.readouterr().err
        assert code == 2, (m, err)
        assert "internal error" not in err and "Traceback" not in err and T.BACKSTOP not in err
        lines = err.splitlines()
        for where, key in damaged:
            tag = f"{where}.{key}" if where else key
            assert any(f": {tag}" in x or f": {where or 'model'}: missing field {key!r}" in x
                       for x in lines), (where, key, err)
        p.unlink()


# ---------------------------------------------------------------------------
# A check refuses what a write refuses, and times reach the longest hit
# ---------------------------------------------------------------------------

def test_check_refuses_the_paths_a_write_refuses(tmp_path):
    """After a good write, the source is made a hard link of the header. A
    write of the same command line refuses the two as one file, so the check
    must not answer current."""
    p, c, h = _generate(tmp_path, tone_model())
    assert run(p, "--c", c, "--h", h, "--prefix", "demo_sfx_", "--check").returncode == 0
    c.unlink()
    try:
        os.link(h, c)
    except OSError:
        pytest.skip("this file system cannot make a hard link")
    for extra in ((), ("--check",), ("--check", "--full")):
        r = run(p, "--c", c, "--h", h, "--prefix", "demo_sfx_", *extra)
        assert r.returncode == 64, (extra, r.stderr)
        assert "Traceback" not in r.stderr


def test_a_time_reaches_the_longest_hit():
    """At 8000 Hz a tone of 160000 samples lasts 20 s: its ramps and envelope
    may use all of it. The cap is the longest hit at the lowest rate."""
    assert T.MAX_US == 32768000
    m = tone_model(len=160000)
    layer = m["sounds"]["beep"]["layers"][0]
    layer.update(len=160000, attack_us=12000000, release_us=8000000,
                 env=[[0, 0], [20000000, 0]])
    assert T.check_model(m) == []
    layer["attack_us"] = T.MAX_US + 1
    assert any("attack_us: must be 0 to 32768000" in f for f in T.check_model(m))


# ---------------------------------------------------------------------------
# A bed under the other sounds
# ---------------------------------------------------------------------------

def _shot(length, peak):
    """A 2000 Hz sine at 8000 Hz for 8 samples, then rest to length."""
    return {"len": length, "peak_steps": peak, "layers": [
        {"kind": "tone", "hits": [[0, 0]], "len": 8, "pitch_mhz": [2000000, 2000000],
         "level_mdb": 0, "partials": [[10000, 0]]}]}


def _bed_model(split=None):
    """A loop of 800 samples, drone, and two one-shots, beep and blip."""
    drone = {"len": 800, "peak_steps": 10, "loop": True, "layers": [
        {"kind": "tone", "hits": [[0, 0]], "len": 800, "pitch_mhz": [2000500, 2000500],
         "level_mdb": 0, "partials": [[10000, 0]]}]}
    if split:
        drone["split"] = split
    return {"format": 1, "rate_hz": 8000, "ceiling_steps": 127,
            "sounds": {"drone": drone, "beep": _shot(9, 8), "blip": _shot(12, 6)}}


def _bed_run(tmp_path, m, *extra):
    p = write_model(tmp_path, m)
    c, h = tmp_path / "sfx_table.c", tmp_path / "sfx_table.h"
    r = run(p, "--c", c, "--h", h, "--prefix", "demo_sfx_", *extra)
    return r, p, c, h


def test_bed_heads_and_rests(tmp_path):
    m = _bed_model()
    r, p, c, h = _bed_run(tmp_path, m, "--bed", "drone", "--dump", tmp_path / "d")
    assert r.returncode == 0, r.stderr
    built = T.build(m)
    bed = built["drone"]
    ht = h.read_text()
    assert '#define DEMO_SFX_BED_NAME "drone"' in ht
    assert "#define DEMO_SFX_BED_HEAD_MAX 12u" in ht
    for name in ("beep", "blip"):
        shot = built[name]
        want = bytes(min(255, max(0, shot[i] + bed[i] - 128)) for i in range(len(shot)))
        head = (tmp_path / "d" / f"{name}_head.u8").read_bytes()
        rest = (tmp_path / "d" / f"{name}_rest.u8").read_bytes()
        assert head == want
        assert rest == bed[len(shot):800]
        un = "DEMO_SFX_" + name.upper()
        assert f"#define {un}_HEAD_LEN {len(head)}u" in ht
        assert f"#define {un}_HEAD_CRC32 0x{zlib.crc32(head):08x}u" in ht
        assert f"#define {un}_REST_LEN {len(rest)}u" in ht
        assert f"#define {un}_REST_CRC32 0x{zlib.crc32(rest):08x}u" in ht
    # The bed is a loop, so it gets no head; the C file gains nothing.
    assert "DRONE_HEAD" not in ht and not (tmp_path / "d" / "drone_head.u8").exists()
    with_bed = c.read_bytes()
    r, *_ = _bed_run(tmp_path, m)
    assert r.returncode == 0 and c.read_bytes() == with_bed


def test_bed_with_split_rests_end_at_the_first_part(tmp_path):
    m = _bed_model(split=300)
    r, *_ = _bed_run(tmp_path, m, "--bed", "drone", "--dump", tmp_path / "d")
    assert r.returncode == 0, r.stderr
    bed = T.build(m)["drone"]
    assert (tmp_path / "d" / "beep_rest.u8").read_bytes() == bed[9:300]
    assert (tmp_path / "d" / "blip_rest.u8").read_bytes() == bed[12:300]


def test_bed_over_bed_wav(tmp_path):
    m = _bed_model()
    p = write_model(tmp_path, m)
    r = run(p, "--bed", "drone", "--wav", tmp_path / "w", "--dump", tmp_path / "d")
    assert r.returncode == 0, r.stderr
    built = T.build(m)
    head = (tmp_path / "d" / "beep_head.u8").read_bytes()
    rest = (tmp_path / "d" / "beep_rest.u8").read_bytes()
    with wave.open(str(tmp_path / "w" / "beep_over_bed.wav")) as w:
        assert w.readframes(w.getnframes()) == head + rest + built["drone"]
    assert not (tmp_path / "w" / "drone_over_bed.wav").exists()


def _with(m, name, **fields):
    m["sounds"][name].update(fields)
    return m


@pytest.mark.parametrize("bed,model,code,words", [
    ("nope", _bed_model(), 64, "the model has no such sound"),
    ("beep", _bed_model(), 64, "is not a loop"),
    ("drone", _with(_bed_model(), "blip", len=800), 2, "must be under 800"),
    ("drone", _with(_bed_model(split=300), "blip", len=300), 2, "must be under 300"),
    ("drone", _with(_bed_model(), "beep", peak_steps=118), 2, "add up to 128, over 127"),
])
def test_bed_refusals(tmp_path, bed, model, code, words):
    r, *_ = _bed_run(tmp_path, model, "--bed", bed, "--dump", tmp_path / "d")
    assert r.returncode == code, r.stderr
    assert words in r.stderr and "Traceback" not in r.stderr
    assert sorted(f.name for f in tmp_path.iterdir()) == ["model.json"]


def test_bed_peak_sum_of_127_is_taken(tmp_path):
    r, *_ = _bed_run(tmp_path, _with(_bed_model(), "beep", peak_steps=117), "--bed", "drone")
    assert r.returncode == 0, r.stderr


def test_check_sees_the_bed(tmp_path):
    m = _bed_model()
    m["sounds"]["drone2"] = copy.deepcopy(m["sounds"]["drone"])
    r, p, c, h = _bed_run(tmp_path, m)
    assert r.returncode == 0, r.stderr
    args = ("--c", c, "--h", h, "--prefix", "demo_sfx_", "--check")
    assert run(p, *args).returncode == 0
    r = run(p, *args, "--bed", "drone")
    assert r.returncode == 1 and "made without --bed, checked with --bed drone" in r.stderr
    assert _bed_run(tmp_path, m, "--bed", "drone")[0].returncode == 0
    assert run(p, *args, "--bed", "drone").returncode == 0
    assert run(p, *args, "--bed", "drone", "--full").returncode == 0
    r = run(p, *args)
    assert r.returncode == 1 and "made with --bed drone, checked without --bed" in r.stderr
    r = run(p, *args, "--bed", "drone2")
    assert r.returncode == 1 and "checked with --bed drone2" in r.stderr
    ht = h.read_text()
    crc = re.search(r"DEMO_SFX_BEEP_HEAD_CRC32 (0x[0-9a-f]{8}u)", ht).group(1)
    h.write_text(ht.replace(crc, "0x00000000u"))
    assert run(p, *args, "--bed", "drone").returncode == 0     # the fast check
    r = run(p, *args, "--bed", "drone", "--full")
    assert r.returncode == 1 and "differs from a new render" in r.stderr


@pytest.mark.parametrize("clash,option", [
    ("beep_head", "--dump"), ("beep_rest", "--dump"), ("beep_over_bed", "--wav"),
    ("beep_head", "--c")])
def test_bed_name_clashes_are_refused(tmp_path, clash, option):
    m = _bed_model()
    m["sounds"][clash] = _shot(10, 5)
    p = write_model(tmp_path, m)
    if option == "--c":
        r = run(p, "--c", tmp_path / "sfx_t.c", "--h", tmp_path / "sfx_t.h", "--prefix", "x_sfx_",
                "--bed", "drone")
    else:
        r = run(p, option, tmp_path / "d", "--bed", "drone")
    assert r.returncode == 64, r.stderr
    assert "already given by" in r.stderr
    assert sorted(f.name for f in tmp_path.iterdir()) == ["model.json"]
    # Without --bed the same model is fine.
    assert run(p, option if option != "--c" else "--dump", tmp_path / "d").returncode == 0


def test_without_a_bed_nothing_mentions_a_head(tmp_path):
    m = _bed_model()
    r, _, c, h = _bed_run(tmp_path, m, "--dump", tmp_path / "d", "--wav", tmp_path / "w")
    assert r.returncode == 0, r.stderr
    for f in (c, h):
        text = f.read_text()
        assert "HEAD" not in text and "REST" not in text and "BED" not in text
    names = os.listdir(tmp_path / "d") + os.listdir(tmp_path / "w")
    assert not [n for n in names if "_head" in n or "_rest" in n or "over_bed" in n]


@pytest.mark.skipif(shutil.which("cc") is None, reason="no host C compiler")
def test_generated_c_compiles_with_a_bed(tmp_path):
    m = _bed_model(split=300)
    r, _, c, h = _bed_run(tmp_path, m, "--bed", "drone")
    assert r.returncode == 0, r.stderr
    main = tmp_path / "main.c"
    main.write_text(
        '#include "sfx_table.h"\n'
        "static uint8_t heads[DEMO_SFX_BED_HEAD_MAX];\n"
        "_Static_assert(DEMO_SFX_BEEP_HEAD_LEN + DEMO_SFX_BEEP_REST_LEN"
        " == DEMO_SFX_DRONE_1_LEN, \"len\");\n"
        "int main(void)\n{\n"
        "    heads[0] = demo_sfx_drone_1[0];\n"
        "    return heads[0] == 128 && DEMO_SFX_BEEP_REST_CRC32 != 0u ? 0 : 1;\n}\n")
    exe = tmp_path / "a.out"
    r = subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-I", str(tmp_path),
                        str(c), str(main), "-o", str(exe)],
                       capture_output=True, text=True, env=minimal_env())
    assert r.returncode == 0, r.stderr
    assert subprocess.run([str(exe)], env=minimal_env()).returncode == 0


def test_a_long_bed_under_many_one_shots_stays_small(tmp_path):
    # A rest is a slice of the bed. The tool used to copy it for each
    # one-shot: this model took about 1.5 GB. Now it takes about 60 MB.
    bed = {"len": 262144, "peak_steps": 10, "loop": True, "layers": [
        {"kind": "tone", "hits": [[0, 0]], "len": 262144, "pitch_mhz": [2000000, 2000000],
         "level_mdb": 0, "partials": [[10000, 0]]}]}
    sounds = {"drone": bed}
    for i in range(5000):
        sounds[f"s{i}"] = _shot(9, 8)
    p = write_model(tmp_path, {"format": 1, "rate_hz": 8000, "ceiling_steps": 127,
                               "sounds": sounds})
    # ru_maxrss of the children, read in a parent that does nothing else.
    probe = ("import resource, subprocess, sys\n"
             "r = subprocess.run(sys.argv[1:])\n"
             "print(r.returncode, resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss)\n")
    r = subprocess.run([sys.executable, "-I", "-c", probe, sys.executable, "-I", TOOL, str(p),
                        "--c", str(tmp_path / "sfx_t.c"), "--h", str(tmp_path / "sfx_t.h"),
                        "--prefix", "x_sfx_", "--bed", "drone"],
                       capture_output=True, text=True, env=minimal_env(), timeout=60)
    code, rss = map(int, r.stdout.split())
    assert code == 0, r.stderr
    peak = rss if sys.platform == "darwin" else rss * 1024
    assert peak < 300 * 1024 * 1024, peak


@pytest.mark.parametrize("bed,words", [
    ("nope", "--bed 'nope': the model has no such sound"),
    ("beep", "--bed 'beep': the sound is not a loop")])
@pytest.mark.parametrize("made", [None, (), ("--bed", "drone")])
def test_check_refuses_a_bad_bed_as_a_write_does(tmp_path, bed, words, made):
    m = _bed_model()
    p = write_model(tmp_path, m)
    c, h = tmp_path / "sfx_table.c", tmp_path / "sfx_table.h"
    table = ("--c", c, "--h", h, "--prefix", "demo_sfx_")
    if made is not None:
        assert run(p, *table, *made).returncode == 0
    wrote = run(p, *table, "--bed", bed)
    checked = run(p, *table, "--check", "--bed", bed)
    assert wrote.returncode == checked.returncode == 64, checked.stderr
    assert checked.stderr == wrote.stderr == f"sfx_table.py: error: {words}\n"
