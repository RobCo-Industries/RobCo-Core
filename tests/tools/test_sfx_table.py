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
                 (tmp_path / "a\nb.json", "--c", c, "--h", h, "--prefix", "x_"),
                 (tmp_path / "x??=.json", "--c", c, "--h", h, "--prefix", "x_"),
                 (tmp_path / "a b.json", "--c", c, "--h", h, "--prefix", "x_"),
                 (tmp_path / "a*b.json", "--c", c, "--h", h, "--prefix", "x_"),
                 (tmp_path / "-m.json", "--c", c, "--h", h, "--prefix", "x_")):
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
    # A guard ends in _H now, so these clashes cannot come from the command
    # line. The check stays as a second line of defence.
    m["sounds"] = {"beep": tone_model()["sounds"]["beep"]}
    for h_name, bad in (("uint8_max", "UINT8_MAX, a name of <stdint.h>"),
                        ("size_max", "SIZE_MAX, a name of <stdint.h>"),
                        ("x_beep_peak", "X_BEEP_PEAK, already given by"),
                        ("x_model_sha256", "X_MODEL_SHA256, already given by")):
        faults = T.c_name_faults(T.c_names(m, {"beep": bytes(8)}, "x_", h_name))
        assert len(faults) == 1 and bad in faults[0]


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
    assert len([x for x in r.stderr.splitlines() if "warning:" not in x]) == 1
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
    ("[" * 100000 + "]" * 100000, "model: must be an object"),
], ids=["inf", "-inf", "long", "decimal", "deep"])
def test_refusal_of_bad_numbers(tmp_path, text, message):
    p = tmp_path / "m.json"
    p.write_text(text)
    r = run(p, "--dump", tmp_path / "d")
    assert r.returncode == 2 and message in r.stderr and "Traceback" not in r.stderr


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
    ("--h", "sönd.h", "the file name must be"),
    ("--h", "x q.h", "the file name must be"),
    ("--h", "x\\q.h", "the file name must be"),
    ("--h", "x'q.h", "the file name must be"),
    ("--h", "x*q.h", "the file name must be"),
    ("--h", "1.h", "the file name must be"),
    ("--h", "_x.h", "the file name must be"),
    ("--h", "-x.h", "the file name must be"),
    ("--h", ".h", "the file name must be"),
    ("--h", "x.hpp", "the file name must be"),
    ("--h", "x_rate_hz", "the file name must be"),
    ("--h", "x.h\n", "the file name must be"),
    ("--h", "stdint.h", "a standard C or POSIX header"),
    ("--h", "STDINT.H", "the file name must be"),
    ("--h", "Stdint.h", "a standard C or POSIX header"),
    ("--h", "stdbit.h", "a standard C or POSIX header"),
    ("--h", "string.h", "a standard C or POSIX header"),
    ("--h", "unistd.h", "a standard C or POSIX header"),
    ("--h", "pthread.h", "a standard C or POSIX header"),
    ("--c", 'x"q.c', "the file name must be"),
    ("--c", "out.h", "the file name must be"),
    ("--c", "a b.c", "the file name must be"),
    ("--c", "1.c", "the file name must be"),
    ("--c", "sönd.c", "the file name must be"),
]


@pytest.mark.parametrize("option,name,message", BAD_FILE_NAMES,
                         ids=[f"{o}-{n!r}" for o, n, _ in BAD_FILE_NAMES])
@pytest.mark.parametrize("check", [False, True])
def test_bad_file_names_are_refused(tmp_path, option, name, message, check):
    p = write_model(tmp_path, tone_model())
    names = {"--c": "t.c", "--h": "t.h", option: name}
    args = [p, "--c", tmp_path / names["--c"], "--h", tmp_path / names["--h"], "--prefix", "x_"]
    r = run(*args, *(["--check"] if check else []))
    assert r.returncode == 64, r.stderr
    errors = [line for line in r.stderr.splitlines() if "error:" in line]
    assert len(errors) == 1 and message in errors[0] and option in errors[0]
    assert "Traceback" not in r.stderr
    assert sorted(f.name for f in tmp_path.iterdir()) == ["model.json"]


# The test's own copy of the standard header names, written apart from the
# tool's table: C23 (ISO/IEC 9899:2024, 7.1.2), the plain file names of the
# POSIX.1-2024 headers index, older POSIX and SUS issues, and System V ar.h.
C23_NAMES = """assert complex ctype errno fenv float inttypes iso646 limits locale
    math setjmp signal stdalign stdarg stdatomic stdbit stdbool stdckdint
    stddef stdint stdio stdlib stdnoreturn string tgmath threads time uchar
    wchar wctype"""
POSIX_NAMES = """aio assert complex cpio ctype devctl dirent dlfcn endian errno
    fcntl fenv float fmtmsg fnmatch ftw glob grp iconv inttypes iso646
    langinfo libintl libgen limits locale math monetary mqueue ndbm netdb
    nl_types poll pthread pwd regex sched search semaphore setjmp signal
    spawn stdalign stdarg stdatomic stdbool stddef stdint stdio stdlib
    stdnoreturn string strings syslog tar termios tgmath threads time uchar
    unistd utmpx wchar wctype wordexp"""
OLDER_NAMES = "re_comp regexp stropts trace ucontext ulimit utime varargs ar"


def test_header_table_is_sorted_and_whole():
    table = T.STANDARD_HEADERS
    assert list(table) == sorted(table), "not sorted"
    assert len(set(table)) == len(table), "a duplicate"
    c23 = {n + ".h" for n in C23_NAMES.split()}
    assert len(c23) == 31
    mine = c23 | {n + ".h" for n in (POSIX_NAMES + " " + OLDER_NAMES).split()}
    assert mine == set(table), (sorted(mine - set(table)), sorted(set(table) - mine))


@pytest.mark.parametrize("name", ["ar.h", "AR.h", "ucontext.h", "stdbit.h", "wordexp.h"])
def test_listed_header_names_are_refused(tmp_path, name):
    p = write_model(tmp_path, tone_model())
    r = run(p, "--c", tmp_path / "t.c", "--h", tmp_path / name, "--prefix", "x_")
    assert r.returncode == 64 and "a standard C or POSIX header" in r.stderr


def test_header_without_the_prefix_warns(tmp_path):
    p = write_model(tmp_path, tone_model())
    r = run(p, "--c", tmp_path / "t.c", "--h", tmp_path / "t.h", "--prefix", "x_")
    assert r.returncode == 0 and "warning: --h 't.h' does not start with" in r.stderr
    r = run(p, "--c", tmp_path / "t.c", "--h", tmp_path / "x_t.h", "--prefix", "x_")
    assert r.returncode == 0 and r.stderr == ""


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
    p = write_model(tmp_path, tone_model(), name="model.c")
    sub = tmp_path / "sub"
    sub.mkdir()
    c, h = tmp_path / "t.c", tmp_path / "t.h"
    if how == "model as --c":
        return [p, "--c", p, "--h", h, "--prefix", "x_"]
    if how == "relative and absolute":
        return [p, "--c", os.path.relpath(p), "--h", h, "--prefix", "x_"]
    if how == "dot and dot-dot":
        return [p, "--c", sub / ".." / "." / "model.c", "--h", h, "--prefix", "x_"]
    if how == "symbolic link":
        (sub / "link.c").symlink_to(p)
        return [p, "--c", sub / "link.c", "--h", h, "--prefix", "x_"]
    if how == "hard link":
        try:
            os.link(p, sub / "hard.c")
        except OSError as e:
            pytest.skip(f"this file system makes no hard link: {e}")
        return [p, "--c", sub / "hard.c", "--h", h, "--prefix", "x_"]
    if how == "case":
        return [p, "--c", tmp_path / "MODEL.c", "--h", h, "--prefix", "x_"]
    if how == "header link to source":
        (sub / "x_t.h").symlink_to(c)
        c.write_text("old\n")
        return [p, "--c", c, "--h", sub / "x_t.h", "--prefix", "x_"]
    if how == "--c is a directory":
        (sub / "d.c").mkdir()
        return [p, "--c", sub / "d.c", "--h", h, "--prefix", "x_"]
    if how == "missing folder":
        return [p, "--c", tmp_path / "none" / "t.c", "--h", h, "--prefix", "x_"]
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
    c, h = tmp_path / "x_t.c", tmp_path / "x_t.h"
    d, w = tmp_path / "d", tmp_path / "w"
    args = [str(p), "--c", str(c), "--h", str(h), "--prefix", "x_",
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
    args = [str(p), "--c", str(tmp_path / "x_t.c"), "--h", str(tmp_path / "x_t.h"),
            "--prefix", "x_", "--dump", str(tmp_path / "d"), "--wav", str(tmp_path / "w")]
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
    p = write_model(tmp_path, tone_model(), name="model.c")
    old = p.read_bytes()
    r = subprocess.run([sys.executable, "-I", TOOL, "model.c", "--c", "model.c", "--h",
                        "x_t.h", "--prefix", "x_"], capture_output=True, text=True,
                       env=minimal_env(), cwd=tmp_path)
    assert r.returncode == 64 and "MODEL 'model.c' and --c 'model.c'" in r.stderr
    assert p.read_bytes() == old and sorted(os.listdir(tmp_path)) == ["model.c"]


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
