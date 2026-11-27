#!/usr/bin/env python3
"""Build 8-bit sound tables from a sound effect model. See docs/spec/SFX_MODEL.md.

    sfx_table.py MODEL --c OUT.c --h OUT.h --prefix NAME_
    sfx_table.py MODEL --c OUT.c --h OUT.h --prefix NAME_ --check [--full]
    sfx_table.py MODEL --dump DIR
    sfx_table.py MODEL --wav DIR

--dump and --wav may go with the first form. --check writes nothing: it
compares the model's SHA-256 and the tool revision recorded in OUT.h and in
the comment of OUT.c. With --full it renders every sound again and compares
both files byte for byte. Files are written to a temporary name and renamed,
OUT.h last.

Exit codes: 0 success, 1 a file is out of date or cannot be read or written,
2 the model is refused, 64 usage error.

Stdlib only. Runs with python3 -I.
"""

import argparse
import hashlib
import io
import json
import os
import re
import sys
import wave
import zlib

# The revision of the algorithm. Raise it when any output byte can change.
REVISION = 1
FORMAT = 1

EX_STALE = 1
EX_MODEL = 2
EX_USAGE = 64

# ---------------------------------------------------------------------------
# Arithmetic
#
# Every value below comes from IEEE doubles with add, subtract, multiply,
# divide and compare, and from exact conversions between int and float.
# The tool imports no math module and uses no ** and no built-in sum() on
# floats. Why: a platform's math.sin or math.exp may differ in its last bit
# from another's, and one bit can flip the rounding of a sample. sum() of
# floats changed to a compensated sum in Python 3.12. The four functions
# here run the same operations in the same order everywhere, so one model
# gives the same bytes on every machine and every Python version.
# Decimal literals are exact: Python parses them with correct rounding.
# ---------------------------------------------------------------------------

PI = 3.141592653589793
TWO_PI = 6.283185307179586
HALF_PI = 1.5707963267948966
TWO_OVER_PI = 0.6366197723675814
# pi / 2 in three parts of 33 bits each, so k * part is exact for k < 2^20.
PIO2_1 = 1.57079632673412561417e+00
PIO2_2 = 6.07710050630396597660e-11
PIO2_3 = 2.02226624871116645580e-21
SINE_LIMIT = 1600000.0          # just over 2^20 * pi / 2
# ln 2 in two parts. LN2_HI has 32 bits, so e * LN2_HI is exact for |e| < 2^20.
LN2_HI = 6.93147180369123816490e-01
LN2_LO = 1.90821492927058770002e-10
INV_LN2 = 1.4426950408889634
SQRT2 = 1.4142135623730951
SQRT1_2 = 0.7071067811865476


def _inverse_factorials(count):
    out, f = [], 1.0
    for k in range(count):
        if k:
            f = f * k
        out.append(1.0 / f)
    return out


_INV_FACT = _inverse_factorials(22)


def _trunc(x):
    """The integer part of x, toward zero. int() of a float is exact."""
    return int(x)


def round_half_away(x):
    """The nearest integer to x, a half away from zero. Exact."""
    a = -x if x < 0 else x
    n = _trunc(a)
    if a - n >= 0.5:
        n += 1
    return -n if x < 0 else n


def _pow2(k):
    """2^k as a float, by exact doubling or halving."""
    r = 1.0
    while k > 0:
        r *= 2.0
        k -= 1
    while k < 0:
        r *= 0.5
        k += 1
    return r


# Taylor coefficients with their signs: -1/3!, +1/5!, ... and -1/2!, +1/4!, ...
_SIN_C = [_INV_FACT[2 * k + 1] if k % 2 == 0 else -_INV_FACT[2 * k + 1] for k in range(1, 10)]
_COS_C = [_INV_FACT[2 * k] if k % 2 == 0 else -_INV_FACT[2 * k] for k in range(1, 11)]


def _sin_poly(r):
    # Taylor sum to r^19 / 19!, by Horner in r^2. For |r| <= pi/4 the first
    # dropped term is under 1e-21.
    r2 = r * r
    p = _SIN_C[-1]
    for c in reversed(_SIN_C[:-1]):
        p = c + r2 * p
    return r + r * r2 * p


def _cos_poly(r):
    # Taylor sum to r^20 / 20!, by Horner in r^2.
    r2 = r * r
    p = _COS_C[-1]
    for c in reversed(_COS_C[:-1]):
        p = c + r2 * p
    return 1.0 + r2 * p


def _reduce(x):
    """(r, k): x = k * pi/2 + r with |r| <= pi/4 plus a few ulp."""
    q = x * TWO_OVER_PI
    k = round_half_away(q)
    r = x - k * PIO2_1
    r = r - k * PIO2_2
    r = r - k * PIO2_3
    return r, k


def sin(x):
    """Sine of x for |x| <= 1.6e6.

    Method: reduce x to r in [-pi/4, pi/4] and a quarter turn k with pi/2 in
    three parts (Cody and Waite), then a fixed Taylor sum of sin r or cos r.
    Error bound: absolute error under 5e-16 for every allowed x.
    """
    if not -SINE_LIMIT <= x <= SINE_LIMIT:
        raise ValueError("sine argument out of range")
    r, k = _reduce(x)
    k &= 3
    if k == 0:
        return _sin_poly(r)
    if k == 1:
        return _cos_poly(r)
    if k == 2:
        return -_sin_poly(r)
    return -_cos_poly(r)


def cos(x):
    """Cosine of x for |x| <= 1.6e6. Same method and bound as sin()."""
    if not -SINE_LIMIT <= x <= SINE_LIMIT:
        raise ValueError("cosine argument out of range")
    r, k = _reduce(x)
    k &= 3
    if k == 0:
        return _cos_poly(r)
    if k == 1:
        return -_sin_poly(r)
    if k == 2:
        return -_cos_poly(r)
    return _sin_poly(r)


def ln(x):
    """Natural log of x > 0.

    Method: scale x by exact powers of 2 to m in [sqrt(1/2), sqrt(2)), then
    ln m = 2 atanh(s) with s = (m - 1) / (m + 1), |s| < 0.172, summed to
    s^25 / 25. Add e * ln 2 with ln 2 in two parts.
    Error bound: under 4 ulp of the result plus 2e-16 absolute.
    """
    if not x > 0.0:
        raise ValueError("log of a value that is not positive")
    e = 0
    while x >= SQRT2:
        x *= 0.5
        e += 1
    while x < SQRT1_2:
        x *= 2.0
        e -= 1
    s = (x - 1.0) / (x + 1.0)
    s2 = s * s
    p = 1.0 / 25.0
    for k in (23, 21, 19, 17, 15, 13, 11, 9, 7, 5, 3, 1):
        p = 1.0 / k + s2 * p
    t = 2.0 * s * p
    return e * LN2_HI + (e * LN2_LO + t)


LN10 = ln(10.0)


def exp(x):
    """e to the power x, for x <= 709. Returns 0.0 under -745.

    Method: x = k ln 2 + r with |r| <= 0.35 and ln 2 in two parts, a fixed
    Taylor sum of e^r to r^20 / 20!, then exact doubling or halving k times.
    Error bound: under 3 ulp of the result for x in [-700, 700].
    """
    if x > 709.0:
        raise ValueError("exp overflow")
    if x < -745.0:
        return 0.0
    k = round_half_away(x * INV_LN2)
    r = (x - k * LN2_HI) - k * LN2_LO
    p = _INV_FACT[20]
    for j in range(19, -1, -1):
        p = _INV_FACT[j] + r * p
    return p * _pow2(k)


def sqrt(x):
    """Square root of x >= 0.

    Method: scale x by exact powers of 4 to m in [1, 4), start from
    (1 + m) / 2 and take 6 Newton steps y = (y + m / y) / 2, then scale back.
    Five steps reach 1e-28 in exact arithmetic, the sixth settles rounding.
    Error bound: under 1 ulp. Exact for a square of a short integer.
    """
    if x < 0.0:
        raise ValueError("square root of a negative value")
    if x == 0.0:
        return 0.0
    e = 0
    while x >= 4.0:
        x *= 0.25
        e += 1
    while x < 1.0:
        x *= 4.0
        e -= 1
    y = 0.5 * (1.0 + x)
    for _ in range(6):
        y = 0.5 * (y + x / y)
    return y * _pow2(e)


def db_gain(db):
    """The amplitude of a level in dB: e^(db * ln 10 / 20).

    Every level the tool passes is a sum of at most two checked levels, or a
    point between two checked env levels, so db is in -240 to 40 and the
    argument of exp() in -28 to 5.
    """
    return exp(db * LN10 / 20.0)


def ceil_div(a, b):
    """ceil(a / b) for integers, b > 0. Exact."""
    return -((-a) // b)


# ---------------------------------------------------------------------------
# The noise generator and the filter
# ---------------------------------------------------------------------------

MASK = 0xFFFFFFFF
FIR_TAPS = 511
DESIGN_BINS = 1024
NOISE_SCALE = 65536.0 / sqrt(12.0)   # rms of the generator's 16-bit values


def xorshift32(s):
    s ^= (s << 13) & MASK
    s ^= s >> 17
    s ^= (s << 5) & MASK
    return s


def start_state(seed, hit):
    """Generator state for hit number `hit` of a layer."""
    s = (seed * 2654435761 + (hit + 1) * 0x9E3779B9) & MASK
    if s == 0:
        s = 0x1234567
    for _ in range(4):
        s = xorshift32(s)
    return s


def white(state, n):
    """n samples of the generator, unit rms."""
    out = [0.0] * n
    for i in range(n):
        state ^= (state << 13) & MASK
        state ^= state >> 17
        state ^= (state << 5) & MASK
        out[i] = (((state >> 8) & 0xFFFF) - 32768) / NOISE_SCALE
    return out


def curve_db(bands, f):
    """Level in dB of the band curve at f Hz, or None outside it."""
    if f < bands[0][0] or f > bands[-1][0]:
        return None
    for (f0, m0), (f1, m1) in zip(bands, bands[1:]):
        if f <= f1:
            d0, d1 = m0 / 1000.0, m1 / 1000.0
            # Validation keeps the frequencies rising, so f1 > f0 here.
            return d0 + (d1 - d0) * (f - f0) / (f1 - f0) if f1 > f0 else d1
    return bands[-1][1] / 1000.0


def fir_design(bands, rate):
    """The 511 taps of a band curve, symmetric, unit power. None if silent."""
    half = DESIGN_BINS // 2
    amp = []
    for k in range(half + 1):
        d = curve_db(bands, k * rate / DESIGN_BINS)
        amp.append(0.0 if d is None else db_gain(d))
    # cos() below sees 0 to 2 pi, then 0 to pi: inside its limit.
    cs = [cos(2 * PI * i / DESIGN_BINS) for i in range(DESIGN_BINS)]
    mid = FIR_TAPS // 2
    h = [0.0] * FIR_TAPS
    for n in range(mid + 1):
        v = amp[0] + amp[half] * cs[(half * n) % DESIGN_BINS]
        for k in range(1, half):
            v += 2 * amp[k] * cs[(k * n) % DESIGN_BINS]
        v *= 0.5 + 0.5 * cos(PI * n / (mid + 1))
        h[mid + n] = h[mid - n] = v
    e = 0.0
    for v in h:
        e += v * v
    if e == 0.0:
        return None
    e = sqrt(e)                     # e > 0 here
    return [v / e for v in h]


def convolve_direct(h, w, n):
    """out[i] = h[0] w[i] + h[1] w[i+1] + ..., added in that order.

    The definition. convolve() gives the same bits faster.
    """
    taps = len(h)
    out = [0.0] * n
    for i in range(n):
        acc = 0.0
        for j in range(taps):
            acc += h[j] * w[i + j]
        out[i] = acc
    return out


def convolve(h, w, n):
    """Same bits as convolve_direct(): every output adds the same products
    in the same order, tap 0 first. Only the loop nesting differs."""
    acc = [0.0] * n
    for j, hj in enumerate(h):
        acc = [a + hj * b for a, b in zip(acc, w[j:j + n])]
    return acc


# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------

def env_gain(points, n, rate, hold_last=False):
    """Per-sample gain from [[us, mdB], ...]. Straight lines in dB. Sample i
    belongs to the segment from ceil(start) to ceil(end). Silent outside the
    points, or the last value held."""
    g = [0.0] * n
    for (t0, m0), (t1, m1) in zip(points, points[1:]):
        s0 = t0 * rate / 1000000.0
        s1 = t1 * rate / 1000000.0
        a = ceil_div(t0 * rate, 1000000)
        b = min(n, ceil_div(t1 * rate, 1000000))
        span = s1 - s0
        d0, d1 = m0 / 1000.0, m1 / 1000.0
        for i in range(max(0, a), b):
            g[i] = db_gain(d0 + (d1 - d0) * (i - s0) / span)
    if hold_last and points:
        v = db_gain(points[-1][1] / 1000.0)
        for i in range(max(0, ceil_div(points[-1][0] * rate, 1000000)), n):
            g[i] = v
    return g


def _add(out, start, one, level, loop):
    size = len(out)
    for i, v in enumerate(one):
        if v:
            j = start + i
            if loop:
                out[j % size] += v * level
            elif j < size:
                out[j] += v * level


def tone_cycles(f0, ratio, n, rate):
    """Whole turns of the phase after n samples."""
    c, f = 0.0, f0
    for _ in range(n):
        c += f / rate
        f *= ratio
    return c


def render_tone(layer, sound, out, rate, where):
    loop = sound.get("loop", False)
    n = layer["len"]
    f0 = layer["pitch_mhz"][0] / 1000.0
    f1 = layer["pitch_mhz"][1] / 1000.0
    # f0 and f1 are 0.001 to 24000 Hz, so ln() sees a positive value and
    # exp() an argument under 24 either way.
    ratio = exp(ln(f1 / f0) / n)
    if loop and n == sound["len"]:
        c = tone_cycles(f0, ratio, n, rate)
        whole = round_half_away(c)
        move = whole / c - 1.0 if whole else 1.0
        if not -0.0005 <= move <= 0.0005:
            raise ModelError([f"{where}: a loop tone runs {c:.3f} cycles, and"
                              f" {whole} whole cycles is a move of {move * 100:.3f} %,"
                              " above 0.05 %"])
        f0 = f0 * (whole / c)
    att = layer.get("attack_us", 0) * rate / 1000000.0
    rel = layer.get("release_us", 0) * rate / 1000000.0
    env = env_gain(layer["env"], n, rate, hold_last=True) if "env" in layer else None
    parts = [(p[0] / 10000.0, db_gain((layer["level_mdb"] + p[1]) / 1000.0))
             for p in layer["partials"]]
    nyquist = rate / 2
    one = [0.0] * n
    phase = 0.0
    f = f0
    for i in range(n):
        g = 1.0
        if i < att:
            g = (i + 0.5) / att
        if n - i < rel:
            r = (n - i - 0.5) / rel
            if r < g:
                g = r
        if env is not None:
            g *= env[i]
        v = 0.0
        if g:
            for mult, amp in parts:
                if f * mult < nyquist:
                    # Bounded by validation: see sine_bound().
                    v += amp * sin(phase * mult)
        one[i] = v * g
        phase += TWO_PI * f / rate
        f *= ratio
    for at, hit_mdb in layer["hits"]:
        _add(out, at, one, db_gain(hit_mdb / 1000.0), loop)


def noise_len(layer, rate):
    return ceil_div(layer["env"][-1][0] * rate, 1000000)


def noise_gain(layer, rate):
    """The gain of one noise hit: its envelope, then the fold."""
    n = noise_len(layer, rate)
    gain = env_gain(layer["env"], n, rate)
    fold = layer.get("fold", 0)
    for i in range(fold):
        u = (i + 0.5) / fold        # 0 < u < 1: sin() and cos() see under pi/2
        gain[i] *= sin(HALF_PI * u)
        gain[n - fold + i] *= cos(HALF_PI * u)
    return gain


def render_noise(layer, sound, out, rate, where, direct=False):
    loop = sound.get("loop", False)
    h = fir_design(layer["bands"], rate)
    if h is None:
        raise ModelError([f"{where}.bands: the curve is silent on every bin"])
    gain = noise_gain(layer, rate)
    n = len(gain)
    same = None
    conv = convolve_direct if direct else convolve
    for k, (at, hit_mdb) in enumerate(layer["hits"]):
        if layer.get("repeat") and same is not None:
            one = same
        else:
            w = white(start_state(layer["seed"], k), n + FIR_TAPS - 1)
            y = conv(h, w, n)
            one = [y[i] * gain[i] if gain[i] else 0.0 for i in range(n)]
            same = one
        m = n if loop else min(n, len(out) - at)
        _add(out, at, one[:m], db_gain((layer["level_mdb"] + hit_mdb) / 1000.0), loop)


def render_float(model, name, direct=False):
    """The layers of one sound summed, before scaling. A list of floats."""
    sound = model["sounds"][name]
    rate = model["rate_hz"]
    out = [0.0] * sound["len"]
    for li, layer in enumerate(sound["layers"]):
        where = f"sound {name!r}: layers[{li}]"
        if layer["kind"] == "tone":
            render_tone(layer, sound, out, rate, where)
        else:
            render_noise(layer, sound, out, rate, where, direct)
    return out


def peak_of(x):
    p = 0.0
    for v in x:
        a = -v if v < 0 else v
        if a > p:
            p = a
    return p


def render_steps(model, name, direct=False):
    """One sound scaled so its peak is peak_steps, before rounding. Floats,
    0 is the rest level."""
    x = render_float(model, name, direct)
    peak = peak_of(x)
    if peak == 0.0:
        raise ModelError([f"sound {name!r}: renders to silence"])
    k = model["sounds"][name]["peak_steps"] / peak
    return [v * k for v in x]


def render_sound(model, name, direct=False):
    """One sound as a list of bytes 0 to 255, 128 the rest level. Whole,
    not split. Does not run the checks: see check_sound()."""
    return [128 + round_half_away(v) for v in render_steps(model, name, direct)]


def check_sound(model, name, data):
    """Faults of a rendered sound against the checks of the spec."""
    sound = model["sounds"][name]
    where = f"sound {name!r}"
    faults = []
    ceiling = model["ceiling_steps"]
    worst = 0
    for b in data:
        d = b - 128 if b >= 128 else 128 - b
        if d > worst:
            worst = d
    if worst > ceiling:
        faults.append(f"{where}: a sample is {worst} steps from 128, past ceiling_steps {ceiling}")
    if worst != sound["peak_steps"]:
        faults.append(f"{where}: the peak is {worst} steps, not peak_steps {sound['peak_steps']}")
    if sound.get("loop"):
        if sound["peak_steps"] > 32:
            faults.append(f"{where}: a loop's peak_steps must be 32 or less, not {sound['peak_steps']}")
        step = 0
        for a, b in zip(data, data[1:]):
            d = a - b if a >= b else b - a
            if d > step:
                step = d
        join = data[-1] - data[0] if data[-1] >= data[0] else data[0] - data[-1]
        if join > step:
            faults.append(f"{where}: the loop's join steps by {join}, more than"
                          f" the largest step inside it, {step}")
    else:
        first, last = data[0] - 128, data[-1] - 128
        if not -2 <= first <= 2:
            faults.append(f"{where}: the first sample is {first:+d} steps from 128, more than 2")
        if not -8 <= last <= 8:
            faults.append(f"{where}: the last sample is {last:+d} steps from 128, more than 8")
    return faults


def tables(model, name, data):
    """[(table name, bytes)]: one table, or two parts with split."""
    split = model["sounds"][name].get("split")
    if split is None:
        return [(name, data)]
    return [(name + "_1", data[:split]), (name + "_2", data[split:])]


def build(model):
    """{sound name: bytes} for every sound, checked. Raises ModelError."""
    out, faults = {}, []
    for name in model["sounds"]:
        try:
            data = render_sound(model, name)
        except ModelError as e:
            faults.extend(e.faults)
            continue
        f = check_sound(model, name, data)
        faults.extend(f)
        out[name] = bytes(data)
    if faults:
        raise ModelError(faults)
    return out


# ---------------------------------------------------------------------------
# The model
# ---------------------------------------------------------------------------

class ModelError(Exception):
    def __init__(self, faults):
        super().__init__("\n".join(faults))
        self.faults = faults


MAX_LEN = 262144
MAX_LAYERS = 16
MAX_HITS = 64
MAX_PARTIALS = 16
MAX_ENV = 64
MAX_BANDS = 24
MAX_US = 10000000
MDB_MIN, MDB_MAX = -120000, 20000
NAME = re.compile(r"[a-z][a-z0-9_]{0,31}\Z")
# A prefix starts with a letter: a leading _ would make the upper-case macro
# names and the table names reserved identifiers in C.
PREFIX = re.compile(r"[a-z][a-z0-9_]{0,31}\Z")
GUARD = re.compile(r"[A-Z][A-Z0-9_]*\Z")

TOP_KEYS = {"format", "rate_hz", "ceiling_steps", "sounds"}
SOUND_KEYS = {"len", "peak_steps", "layers", "loop", "split"}
TONE_KEYS = {"kind", "hits", "len", "pitch_mhz", "level_mdb", "partials",
             "attack_us", "release_us", "env"}
NOISE_KEYS = {"kind", "hits", "seed", "level_mdb", "env", "bands", "fold", "repeat"}


class _Checker:
    def __init__(self):
        self.faults = []

    def fault(self, where, text):
        self.faults.append(f"{where}: {text}")

    def keys(self, obj, where, allowed, required):
        if not isinstance(obj, dict):
            self.fault(where, "must be an object")
            return False
        for k in obj:
            if k not in allowed:
                self.fault(f"{where}.{k}" if where else k, "unknown field")
        ok = True
        for k in sorted(required):
            if k not in obj:
                self.fault(where or "model", f"missing field {k!r}")
                ok = False
        return ok

    def int(self, v, where, lo, hi):
        if isinstance(v, bool) or not isinstance(v, int):
            self.fault(where, "must be a whole number")
            return False
        if not lo <= v <= hi:
            self.fault(where, f"must be {lo} to {hi}, not {v}")
            return False
        return True

    def bool(self, v, where):
        if not isinstance(v, bool):
            self.fault(where, "must be true or false")
            return False
        return True

    def pairs(self, v, where, lo, hi):
        if not isinstance(v, list):
            self.fault(where, "must be a list")
            return False
        if not lo <= len(v) <= hi:
            self.fault(where, f"must hold {lo} to {hi} points, not {len(v)}")
            return False
        ok = True
        for i, p in enumerate(v):
            if not isinstance(p, list) or len(p) != 2:
                self.fault(f"{where}[{i}]", "must be a pair of whole numbers")
                ok = False
        return ok


def sine_bound(n, from_mhz, to_mhz, multiple, rate):
    """An upper bound of every sine argument of a tone partial, phase * mult.

    A partial calls sin() only while f * mult < rate / 2. With a pitch that
    rises or holds, every earlier f is no higher, so the argument stays under
    pi * n. With a falling pitch the phase after n samples is at most
    2 pi / rate * (f0 + n * (f0 - f1) / ln(f0 / f1)): the sum of a falling
    geometric series is at most its first term plus its integral. A partial
    that is above half the rate even at f1 never calls sin(). The 1 % margin
    covers the loop move of 0.05 % and the rounding of the phase sum.
    Validation only: no output byte depends on it.
    """
    mult = multiple / 10000.0
    if to_mhz >= from_mhz:
        return PI * n
    f0, f1 = from_mhz / 1000.0, to_mhz / 1000.0
    if f1 * mult * 0.99 >= rate / 2:
        return 0.0
    cycles = f0 + n * (f0 - f1) / ln(f0 / f1)
    return 1.01 * mult * TWO_PI * cycles / rate


def _check_layer(c, layer, li, sound, rate, where):
    w = f"{where}.layers[{li}]"
    if not isinstance(layer, dict):
        c.fault(w, "must be an object")
        return
    kind = layer.get("kind")
    if kind == "tone":
        allowed, required = TONE_KEYS, {"kind", "hits", "len", "pitch_mhz", "level_mdb", "partials"}
    elif kind == "noise":
        allowed, required = NOISE_KEYS, {"kind", "hits", "seed", "level_mdb", "env", "bands"}
    else:
        c.fault(f"{w}.kind", 'must be "tone" or "noise"')
        return
    if not c.keys(layer, w, allowed, required):
        return
    slen = sound.get("len") if isinstance(sound.get("len"), int) else MAX_LEN
    if c.pairs(layer["hits"], f"{w}.hits", 1, MAX_HITS):
        for i, (at, m) in enumerate(layer["hits"]):
            c.int(at, f"{w}.hits[{i}][0]", 0, slen - 1)
            c.int(m, f"{w}.hits[{i}][1]", MDB_MIN, MDB_MAX)
    c.int(layer["level_mdb"], f"{w}.level_mdb", MDB_MIN, MDB_MAX)
    env_ok = "env" in layer and c.pairs(layer["env"], f"{w}.env",
                                        1 if kind == "tone" else 2, MAX_ENV)
    if env_ok:
        last = 0
        for i, (t, m) in enumerate(layer["env"]):
            if c.int(t, f"{w}.env[{i}][0]", 0, MAX_US):
                if t < last:
                    c.fault(f"{w}.env[{i}][0]", "must not come before the point above it")
                    env_ok = False
                last = t
            else:
                env_ok = False
            c.int(m, f"{w}.env[{i}][1]", MDB_MIN, MDB_MAX)
    if kind == "tone":
        n = layer["len"]
        n_ok = c.int(n, f"{w}.len", 1, MAX_LEN)
        p = layer["pitch_mhz"]
        if not isinstance(p, list) or len(p) != 2:
            c.fault(f"{w}.pitch_mhz", "must be [from, to] in millihertz")
        else:
            for i, v in enumerate(p):
                c.int(v, f"{w}.pitch_mhz[{i}]", 1, rate * 500)
        pitch_ok = (isinstance(p, list) and len(p) == 2
                    and all(not isinstance(v, bool) and isinstance(v, int)
                            and 1 <= v <= rate * 500 for v in p))
        top = 0
        if c.pairs(layer["partials"], f"{w}.partials", 1, MAX_PARTIALS):
            for i, (mult, m) in enumerate(layer["partials"]):
                if c.int(mult, f"{w}.partials[{i}][0]", 1, 1000000) and mult > top:
                    top = mult
                c.int(m, f"{w}.partials[{i}][1]", MDB_MIN, MDB_MAX)
        if n_ok and pitch_ok and top:
            bound = sine_bound(n, p[0], p[1], top, rate)
            if bound > SINE_LIMIT:
                c.fault(f"{w}.partials", f"the sine of multiple {top} can reach"
                        f" {bound:.0f} radians, past the limit {SINE_LIMIT:.0f}."
                        " Shorten the glide or lower the multiple.")
        for key in ("attack_us", "release_us"):
            if key in layer and c.int(layer[key], f"{w}.{key}", 0, MAX_US) and n_ok:
                if layer[key] * rate > n * 1000000:
                    c.fault(f"{w}.{key}", "must fit inside the tone's len")
    else:
        c.int(layer["seed"], f"{w}.seed", 0, MASK)
        if c.pairs(layer["bands"], f"{w}.bands", 2, MAX_BANDS):
            last = -1
            for i, (hz, m) in enumerate(layer["bands"]):
                if c.int(hz, f"{w}.bands[{i}][0]", 0, rate // 2):
                    if hz <= last:
                        c.fault(f"{w}.bands[{i}][0]", "must be above the point above it")
                    last = hz
                c.int(m, f"{w}.bands[{i}][1]", MDB_MIN, MDB_MAX)
        if "repeat" in layer:
            c.bool(layer["repeat"], f"{w}.repeat")
        if env_ok:
            n = noise_len(layer, rate)
            if n < 1 or n > MAX_LEN:
                c.fault(f"{w}.env", f"must span 1 to {MAX_LEN} samples, not {n}")
            elif "fold" in layer and c.int(layer["fold"], f"{w}.fold", 0, n // 2):
                pass


def check_model(model):
    """Faults of a parsed model, as a list of messages."""
    c = _Checker()
    if not c.keys(model, "", TOP_KEYS, TOP_KEYS):
        return c.faults
    if isinstance(model["format"], bool) or model["format"] != FORMAT:
        c.fault("format", f"must be {FORMAT}")
        return c.faults
    rate_ok = c.int(model["rate_hz"], "rate_hz", 8000, 48000)
    c.int(model["ceiling_steps"], "ceiling_steps", 1, 127)
    ceiling = model["ceiling_steps"] if isinstance(model["ceiling_steps"], int) else 127
    sounds = model["sounds"]
    if not isinstance(sounds, dict) or not sounds:
        c.fault("sounds", "must be an object with at least one sound")
        return c.faults
    names = set()
    for name, sound in sounds.items():
        where = f"sound {name!r}"
        if not NAME.match(name):
            c.fault(where, "the name must be a to z, 0 to 9 and _, start with a letter,"
                           " at most 32 characters")
        if not c.keys(sound, where, SOUND_KEYS, {"len", "peak_steps", "layers"}):
            continue
        len_ok = c.int(sound["len"], f"{where}.len", 1, MAX_LEN)
        c.int(sound["peak_steps"], f"{where}.peak_steps", 1, ceiling)
        if "loop" in sound:
            c.bool(sound["loop"], f"{where}.loop")
        tabs = [name]
        if "split" in sound:
            if len_ok and c.int(sound["split"], f"{where}.split", 1, sound["len"] - 1):
                tabs = [name + "_1", name + "_2"]
        for t in tabs:
            if t in names:
                c.fault(where, f"the table name {t!r} is taken twice")
            names.add(t)
        layers = sound["layers"]
        if not isinstance(layers, list) or not 1 <= len(layers) <= MAX_LAYERS:
            c.fault(f"{where}.layers", f"must be a list of 1 to {MAX_LAYERS} layers")
            continue
        if not rate_ok:
            continue
        for li, layer in enumerate(layers):
            _check_layer(c, layer, li, sound, model["rate_hz"], where)
    return c.faults


def _no_duplicates(pairs):
    out = {}
    for k, v in pairs:
        if k in out:
            raise ValueError(f"field {k!r} appears twice")
        out[k] = v
    return out


def _no_constant(text):
    raise ValueError(f"{text} is not a number of the format")


def parse_model(text):
    """A model from JSON text, checked. Raises ModelError."""
    try:
        model = json.loads(text, object_pairs_hook=_no_duplicates,
                           parse_constant=_no_constant)
    except ValueError as e:
        raise ModelError([f"not valid JSON: {e}"]) from None
    faults = check_model(model)
    if faults:
        raise ModelError(faults)
    return model


def load_model(path):
    """(model, SHA-256 hex of the file). Raises ModelError or OSError."""
    with open(path, "rb") as f:
        raw = f.read()
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        raise ModelError(["not UTF-8 text"]) from None
    return parse_model(text), hashlib.sha256(raw).hexdigest()


# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------

def _comment(source, sha):
    return ("/*\n"
            " * Generated file. Do not edit.\n"
            f" * Source: {source}\n"
            f" * SHA-256: {sha}\n"
            f" * Generator: tools/sfx_table.py revision {REVISION}\n"
            " */\n")


def header_guard(h_name):
    """The include guard of a header name, or None if it is not a valid,
    unreserved C identifier."""
    guard = re.sub(r"[^A-Z0-9]", "_", h_name.upper())
    return guard if GUARD.match(guard) else None


# Keywords of C89 to C23 (ISO/IEC 9899:2024, 6.4.1), with the C23 spellings of
# the older _Alignas and the like, and the GNU and common extensions asm,
# fortran and typeof that -std=gnu11 accepts.
C_KEYWORDS = frozenset("""
    auto break case char const continue default do double else enum extern
    float for goto if inline int long register restrict return short signed
    sizeof static struct switch typedef union unsigned void volatile while
    _Alignas _Alignof _Atomic _BitInt _Bool _Complex _Decimal128 _Decimal32
    _Decimal64 _Generic _Imaginary _Noreturn _Static_assert _Thread_local
    alignas alignof bool constexpr false nullptr static_assert thread_local
    true typeof typeof_unqual asm fortran
""".split())

# Names that <stdint.h> defines, and the patterns C reserves for it (7.31.10).
STDINT = re.compile(r"(?:u?int\w*_t|U?INT\w*_(?:MAX|MIN|C|WIDTH)"
                    r"|(?:PTRDIFF|SIG_ATOMIC|SIZE|WCHAR|WINT)_(?:MAX|MIN|WIDTH))\Z")


def c_names(model, built, prefix, h_name):
    """[(identifier, where it comes from)] for every name the C files define."""
    up = prefix.upper()
    out = [(header_guard(h_name), f"the include guard of --h {h_name!r}")]
    for s in ("MODEL_SHA256", "TOOL_REVISION", "RATE_HZ"):
        out.append((up + s, f"--prefix {prefix!r}"))
    for name, data in built.items():
        where = f"--prefix {prefix!r} and sound {name!r}"
        out.append((up + name.upper() + "_PEAK", where))
        out.append((up + name.upper() + "_LOOP", where))
        for t, _ in tables(model, name, data):
            out.append((prefix + t, where))
            out.append((up + t.upper() + "_LEN", where))
            out.append((up + t.upper() + "_CRC32", where))
    return out


def c_name_faults(names):
    """One line for each identifier that is a keyword, reserved, a <stdint.h>
    name, or written twice."""
    faults, seen = [], {}
    for ident, where in names:
        if ident in C_KEYWORDS:
            faults.append(f"{where} give {ident}, a C keyword")
        elif ident.startswith("_"):
            faults.append(f"{where} give {ident}, a reserved C name")
        elif STDINT.match(ident):
            faults.append(f"{where} give {ident}, a name of <stdint.h>")
        elif ident in seen:
            faults.append(f"{where} give {ident}, already given by {seen[ident]}")
        seen.setdefault(ident, where)
    return faults


def c_header(model, built, prefix, source, sha, h_name):
    up = prefix.upper()
    guard = header_guard(h_name)
    lines = [_comment(source, sha), f"#ifndef {guard}", f"#define {guard}", "",
             "#include <stdint.h>", "",
             f'#define {up}MODEL_SHA256 "{sha}"',
             f"#define {up}TOOL_REVISION {REVISION}",
             f"#define {up}RATE_HZ {model['rate_hz']}u", ""]
    for name, data in built.items():
        sound = model["sounds"][name]
        un = up + name.upper()
        lines.append(f"#define {un}_PEAK {sound['peak_steps']}u")
        lines.append(f"#define {un}_LOOP {1 if sound.get('loop') else 0}")
        for t, part in tables(model, name, data):
            ut = up + t.upper()
            lines.append(f"#define {ut}_LEN {len(part)}u")
            lines.append(f"#define {ut}_CRC32 0x{zlib.crc32(bytes(part)):08x}u")
            lines.append(f"extern const uint8_t {prefix}{t}[{ut}_LEN];")
        lines.append("")
    lines.append(f"#endif /* {guard} */")
    return "\n".join(lines) + "\n"


def c_source(model, built, prefix, source, sha, h_name):
    up = prefix.upper()
    lines = [_comment(source, sha), f'#include "{h_name}"', ""]
    for name, data in built.items():
        for t, part in tables(model, name, data):
            lines.append(f"const uint8_t {prefix}{t}[{up}{t.upper()}_LEN] = {{")
            for i in range(0, len(part), 12):
                lines.append("    " + ", ".join(f"0x{b:02x}" for b in part[i:i + 12]) + ",")
            lines.append("};")
            lines.append("")
    return "\n".join(lines[:-1]) + "\n"


def wav_bytes(data, rate):
    """An 8-bit mono WAV file of the samples, as bytes."""
    buf = io.BytesIO()
    w = wave.open(buf, "wb")
    try:
        w.setnchannels(1)
        w.setsampwidth(1)
        w.setframerate(rate)
        w.writeframes(bytes(data))
    finally:
        w.close()
    return buf.getvalue()


def _temp_for(path):
    """Open a new temporary file next to path. Returns (name, fd)."""
    folder, base = os.path.split(path)
    tmp = os.path.join(folder, f".{base}.{os.getpid()}.tmp")
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o666)
    return tmp, fd


def publish(items, undo):
    """Write each (path, bytes) to a temporary file in the same folder, then
    rename each over its path in order. On a failure, remove the temporary
    files and, with undo, the paths this call already renamed, then raise
    the OSError. A path is never left half written."""
    temps, done = [], []
    try:
        for path, data in items:
            tmp, fd = _temp_for(path)
            temps.append((tmp, path))
            with os.fdopen(fd, "wb") as f:
                f.write(data)
        for tmp, path in temps:
            os.replace(tmp, path)
            done.append(path)
    except OSError:
        for tmp, _ in temps:
            try:
                os.unlink(tmp)
            except OSError:
                pass
        if undo:
            for path in done:
                try:
                    os.unlink(path)
                except OSError:
                    pass
        raise


def recorded(h_text, prefix):
    """(SHA-256, revision) recorded in a generated header, or None."""
    up = re.escape(prefix.upper())
    s = re.search(rf'^#define {up}MODEL_SHA256 "([0-9a-f]{{64}})"$', h_text, re.M)
    r = re.search(rf"^#define {up}TOOL_REVISION (\d+)$", h_text, re.M)
    if not s or not r:
        return None
    return s.group(1), int(r.group(1))


# ---------------------------------------------------------------------------
# Command line
# ---------------------------------------------------------------------------

class Parser(argparse.ArgumentParser):
    def error(self, message):
        self.print_usage(sys.stderr)
        print(f"{self.prog}: error: {message}", file=sys.stderr)
        sys.exit(EX_USAGE)


def _read_bytes(path):
    with open(path, "rb") as f:
        return f.read()


def _text(raw):
    """Generated text for the fast check: UTF-8, any line end read as LF.
    None if it is not UTF-8."""
    try:
        return raw.decode("utf-8").replace("\r\n", "\n")
    except UnicodeDecodeError:
        return None


def commented(text):
    """(SHA-256, revision) in the comment that opens a generated file, or None."""
    m = re.match(r"/\*\n(?: \*[^\n]*\n)*? \* SHA-256: ([0-9a-f]{64})\n"
                 r" \* Generator: tools/sfx_table\.py revision (\d+)\n \*/\n", text)
    return (m.group(1), int(m.group(2))) if m else None


def _check(args, model_path):
    try:
        h_raw = _read_bytes(args.h)
        c_raw = _read_bytes(args.c)
        sha = hashlib.sha256(_read_bytes(model_path)).hexdigest()
    except OSError as e:
        print(f"sfx_table.py: {e.filename}: {e.strerror}", file=sys.stderr)
        return EX_STALE
    h_text, c_text = _text(h_raw), _text(c_raw)
    rec = recorded(h_text, args.prefix) if h_text is not None else None
    if rec is None:
        print(f"sfx_table.py: {args.h}: no {args.prefix.upper()}MODEL_SHA256 or"
              " TOOL_REVISION line", file=sys.stderr)
        return EX_STALE
    if rec[0] != sha:
        print(f"sfx_table.py: {args.h}: the model changed since the table was made."
              " Run sfx_table.py again.", file=sys.stderr)
        return EX_STALE
    if rec[1] != REVISION:
        print(f"sfx_table.py: {args.h}: made by revision {rec[1]}, this tool is"
              f" revision {REVISION}. Run sfx_table.py again.", file=sys.stderr)
        return EX_STALE
    if (c_text is None or commented(c_text) != rec
            or commented(h_text) != rec):
        print(f"sfx_table.py: {args.c}: its comment does not name the same model"
              " SHA-256 and revision as the header. Run sfx_table.py again.",
              file=sys.stderr)
        return EX_STALE
    if args.full:
        model, _ = load_model(model_path)
        built = build(model)
        source = os.path.basename(model_path)
        h_name = os.path.basename(args.h)
        h_new = c_header(model, built, args.prefix, source, sha, h_name)
        if h_new.encode("ascii") != h_raw:
            print(f"sfx_table.py: {args.h}: differs from a new render", file=sys.stderr)
            return EX_STALE
        if c_source(model, built, args.prefix, source, sha, h_name).encode("ascii") != c_raw:
            print(f"sfx_table.py: {args.c}: differs from a new render", file=sys.stderr)
            return EX_STALE
    return 0


def main(argv=None):
    p = Parser(prog="sfx_table.py", description=__doc__.splitlines()[0])
    p.add_argument("model", metavar="MODEL")
    p.add_argument("--c", metavar="OUT.c")
    p.add_argument("--h", metavar="OUT.h")
    p.add_argument("--prefix", metavar="NAME_")
    p.add_argument("--check", action="store_true",
                   help="compare the recorded SHA-256 and revision, write nothing")
    p.add_argument("--full", action="store_true", help="with --check: render and compare bytes")
    p.add_argument("--dump", metavar="DIR", help="write each table as a raw .u8 file")
    p.add_argument("--wav", metavar="DIR", help="write each sound as an 8-bit mono WAV")
    args = p.parse_args(argv)

    table = (args.c, args.h, args.prefix)
    if any(v is not None for v in table) and None in table:
        p.error("--c, --h and --prefix go together")
    if args.prefix is not None and not PREFIX.match(args.prefix):
        p.error("--prefix must be a to z, 0 to 9 and _, start with a letter,"
                " at most 32 characters")
    if args.h is not None and header_guard(os.path.basename(args.h)) is None:
        p.error("--h must have a file name that starts with a letter, so its"
                " include guard is a valid C name")
    if args.c is not None and not re.fullmatch(r"[ -~]+", os.path.basename(args.model)):
        p.error("MODEL must have a file name of printable ASCII: it goes into the"
                " comment of each generated file")
    if args.full and not args.check:
        p.error("--full needs --check")
    if args.check and (args.c is None or args.dump or args.wav):
        p.error("--check needs --c, --h and --prefix, and goes alone")
    if args.c is None and not args.dump and not args.wav:
        p.error("nothing to do: give --c, --h and --prefix, --dump or --wav")

    try:
        if args.check:
            return _check(args, args.model)
        model, sha = load_model(args.model)
        built = build(model)
    except ModelError as e:
        for f in e.faults:
            print(f"sfx_table.py: {args.model}: {f}", file=sys.stderr)
        return EX_MODEL
    except OSError as e:
        print(f"sfx_table.py: {e.filename}: {e.strerror}", file=sys.stderr)
        return EX_STALE

    if args.c is not None:
        h_name = os.path.basename(args.h)
        h_text = c_header(model, built, args.prefix, source=os.path.basename(args.model),
                          sha=sha, h_name=h_name)
        faults = c_name_faults(c_names(model, built, args.prefix, h_name))
        if faults:
            p.print_usage(sys.stderr)
            for f in faults:
                print(f"sfx_table.py: error: {f}", file=sys.stderr)
            return EX_USAGE
    try:
        if args.c is not None:
            source = os.path.basename(args.model)
            # The source first, the header last: the header carries the hash.
            publish([(args.c, c_source(model, built, args.prefix, source, sha,
                                       h_name).encode("ascii")),
                     (args.h, h_text.encode("ascii"))], undo=False)
        if args.dump:
            os.makedirs(args.dump, exist_ok=True)
            publish([(os.path.join(args.dump, t + ".u8"), bytes(part))
                     for name, data in built.items()
                     for t, part in tables(model, name, data)], undo=True)
        if args.wav:
            os.makedirs(args.wav, exist_ok=True)
            items = []
            for name, data in built.items():
                times = 3 if model["sounds"][name].get("loop") else 1
                items.append((os.path.join(args.wav, name + ".wav"),
                              wav_bytes(data * times, model["rate_hz"])))
            publish(items, undo=True)
    except OSError as e:
        print(f"sfx_table.py: {e.filename}: {e.strerror}", file=sys.stderr)
        return EX_STALE
    return 0


if __name__ == "__main__":
    sys.exit(main())
