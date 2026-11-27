#!/usr/bin/env python3
"""Build 8-bit sound tables from a sound effect model. See docs/spec/SFX_MODEL.md.

    sfx_table.py MODEL --c OUT.c --h OUT.h --prefix NAME_sfx_
    sfx_table.py MODEL --c OUT.c --h OUT.h --prefix NAME_sfx_ --check [--full]
    sfx_table.py MODEL --dump DIR
    sfx_table.py MODEL --wav DIR

--dump and --wav may go with the first form. The file names of OUT.c and
OUT.h hold sfx, and the prefix ends in sfx_, so every name written is clear
of C's own names. --check writes nothing: it
compares the model's SHA-256 and the tool revision recorded in OUT.h and in
the comment of OUT.c. With --full it renders every sound again and compares
both files byte for byte. A run checks its whole plan first: no path
is two files or both a file and a folder, and no output is MODEL. Then every file is written to a
temporary name and all are renamed, OUT.h last. A failure restores the old
files.

Exit codes: 0 success, 1 a file is out of date or cannot be read or written,
2 the model is refused, 64 usage error, 70 internal error. An internal error
prints one line and no traceback.

Stdlib only. Runs with python3 -I.
"""

import argparse
import collections
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
EX_SOFTWARE = 70

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


class RangeFault(Exception):
    """An argument outside the range of sin, cos, ln, exp or sqrt below.

    A backstop: validation keeps every accepted model inside these ranges.
    The renderer turns it into a model fault that names the sound and layer.
    """

    def __init__(self, name, x):
        super().__init__(f"{name}({x!r}) is outside the range of the tool's {name}")


def _finite(x):
    return x - x == 0.0


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
        raise RangeFault("sin", x)
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
        raise RangeFault("cos", x)
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
    """Natural log of a finite x > 0.

    Method: scale x by exact powers of 2 to m in [sqrt(1/2), sqrt(2)), then
    ln m = 2 atanh(s) with s = (m - 1) / (m + 1), |s| < 0.172, summed to
    s^25 / 25. Add e * ln 2 with ln 2 in two parts.
    Error bound: under 4 ulp of the result plus 2e-16 absolute.
    """
    if not (x > 0.0 and _finite(x)):
        raise RangeFault("ln", x)
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
    """e to the power x, for x <= 709. Returns 0.0 under -745, -inf included.

    Method: x = k ln 2 + r with |r| <= 0.35 and ln 2 in two parts, a fixed
    Taylor sum of e^r to r^20 / 20!, then exact doubling or halving k times.
    Error bound: under 3 ulp of the result for x in [-700, 700].
    """
    if not x <= 709.0:
        raise RangeFault("exp", x)
    if x < -745.0:
        return 0.0
    k = round_half_away(x * INV_LN2)
    r = (x - k * LN2_HI) - k * LN2_LO
    p = _INV_FACT[20]
    for j in range(19, -1, -1):
        p = _INV_FACT[j] + r * p
    return p * _pow2(k)


def sqrt(x):
    """Square root of a finite x >= 0.

    Method: scale x by exact powers of 4 to m in [1, 4), start from
    (1 + m) / 2 and take 6 Newton steps y = (y + m / y) / 2, then scale back.
    Five steps reach 1e-28 in exact arithmetic, the sixth settles rounding.
    Error bound: under 1 ulp. Exact for a square of a short integer.
    """
    if not (x >= 0.0 and _finite(x)):
        raise RangeFault("sqrt", x)
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


def noise_gain(layer, rate, m=None):
    """The gain of one noise hit: its envelope, then the fold. With m, only
    the first m values, each the same as in the whole list."""
    n = noise_len(layer, rate)
    m = n if m is None else m
    gain = env_gain(layer["env"], m, rate)
    fold = layer.get("fold", 0)
    for i in range(fold):
        u = (i + 0.5) / fold        # 0 < u < 1: sin() and cos() see under pi/2
        if i < m:
            gain[i] *= sin(HALF_PI * u)
        if n - fold + i < m:
            gain[n - fold + i] *= cos(HALF_PI * u)
    return gain


def render_noise(layer, sound, out, rate, where, direct=False):
    loop = sound.get("loop", False)
    h = fir_design(layer["bands"], rate)
    if h is None:
        raise ModelError([f"{where}.bands: the curve is silent on every bin"])
    keep = noise_kept(layer, len(out), noise_len(layer, rate), loop)
    gain = noise_gain(layer, rate, max(keep))
    same = None
    conv = convolve_direct if direct else convolve
    for k, (at, hit_mdb) in enumerate(layer["hits"]):
        if layer.get("repeat") and same is not None:
            one = same
        else:
            # Only the kept samples: the generator runs in order, so the
            # first m + 510 values are the same as in a longer run, and
            # each kept output adds the same products in the same order.
            m = max(keep) if layer.get("repeat") else keep[k]
            w = white(start_state(layer["seed"], k), m + FIR_TAPS - 1)
            y = conv(h, w, m)
            one = [y[i] * gain[i] if gain[i] else 0.0 for i in range(m)]
            same = one
        _add(out, at, one[:keep[k]], db_gain((layer["level_mdb"] + hit_mdb) / 1000.0), loop)


def noise_kept(layer, slen, n, loop):
    """The samples of each hit that reach the sound: all n in a loop, which
    wraps the rest, else those before the sound's end."""
    return [n if loop else min(n, slen - at) for at, _ in layer["hits"]]


# The work of a render, in filter products. A sine of a partial takes
# about 18 products of time, so it counts SINE_COST, and so does each
# sample of a sound, which is summed, scaled and rounded. Designing a
# filter takes about DESIGN_COST. One unit is about 42 ns on the
# reference machine, so the budget is about 42 s. See the spec.
SINE_COST = 32
DESIGN_COST = 131072
MAX_COST = 1000000000


def render_cost(model):
    """The work of a render: FIR_TAPS products for each kept noise sample,
    SINE_COST for each tone sample of each partial and for each sample of a
    sound, DESIGN_COST for each noise layer. A repeat layer filters once.
    The model must be valid."""
    rate = model["rate_hz"]
    cost = 0
    for sound in model["sounds"].values():
        cost += sound["len"] * SINE_COST
        for layer in sound["layers"]:
            if layer["kind"] == "tone":
                cost += layer["len"] * len(layer["partials"]) * SINE_COST
                continue
            keep = noise_kept(layer, sound["len"], noise_len(layer, rate),
                              sound.get("loop", False))
            cost += (max(keep) if layer.get("repeat") else sum(keep)) * FIR_TAPS
            cost += DESIGN_COST
    return cost


def render_float(model, name, direct=False):
    """The layers of one sound summed, before scaling. A list of floats."""
    sound = model["sounds"][name]
    rate = model["rate_hz"]
    out = [0.0] * sound["len"]
    for li, layer in enumerate(sound["layers"]):
        where = f"sound {name!r}: layers[{li}]"
        try:
            if layer["kind"] == "tone":
                render_tone(layer, sound, out, rate, where)
            else:
                render_noise(layer, sound, out, rate, where, direct)
        except RangeFault as e:
            raise ModelError([f"{where}: {BACKSTOP}: {e}"]) from None
    return out


# The start of the fault the backstop gives. Validation should refuse such a
# model first, so this text in a fault means a gap in validation.
BACKSTOP = "the arithmetic left its range"


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
MAX_US = MAX_LEN * 1000000 // 8000   # the longest hit at the lowest rate, 32.768 s
MAX_DIGITS = 15                 # no field takes more than 10 digits
MDB_MIN, MDB_MAX = -120000, 20000
NAME = re.compile(r"[a-z][a-z0-9_]{0,31}\Z")
# Every C name the tool writes carries the marker sfx_ or SFX_, and every
# file name it writes carries sfx. No C or POSIX keyword, type, function,
# macro or reserved pattern contains that marker, so no list of taken names
# is needed. Three rounds of such lists each missed a name: size_t, with a
# sound t and the prefix size_, was the last. The prefix ends in sfx_, each
# part before it starts with a letter, at most 32 characters.
PREFIX = re.compile(r"(?=.{4,32}\Z)(?:[a-z][a-z0-9]*_)*sfx_\Z")
GUARD = re.compile(r"[A-Z][A-Z0-9_]*\Z")

# Every field of the format and its JSON type, by section of the spec's
# field tables. The validator reads each field through _Checker.field(),
# which takes the type from here, so no field skips its type check. A test
# compares this list with the tables in docs/spec/SFX_MODEL.md.
FIELDS = {
    "top": {"format": "int", "rate_hz": "int", "ceiling_steps": "int", "sounds": "object"},
    "sound": {"len": "int", "peak_steps": "int", "layers": "list", "loop": "bool",
              "split": "int"},
    "tone": {"kind": "kind", "hits": "pairs", "len": "int", "pitch_mhz": "pair",
             "level_mdb": "int", "partials": "pairs", "attack_us": "int",
             "release_us": "int", "env": "pairs"},
    "noise": {"kind": "kind", "hits": "pairs", "seed": "int", "level_mdb": "int",
              "env": "pairs", "bands": "pairs", "fold": "int", "repeat": "bool"},
}
REQUIRED = {
    "top": {"format", "rate_hz", "ceiling_steps", "sounds"},
    "sound": {"len", "peak_steps", "layers"},
    "tone": {"kind", "hits", "len", "pitch_mhz", "level_mdb", "partials"},
    "noise": {"kind", "hits", "seed", "level_mdb", "env", "bands"},
}
TOP_KEYS = set(FIELDS["top"])
SOUND_KEYS = set(FIELDS["sound"])
TONE_KEYS = set(FIELDS["tone"])
NOISE_KEYS = set(FIELDS["noise"])


def _is_int(v):
    return isinstance(v, int) and not isinstance(v, bool)


# The validator's rule: one line per fault, and every field that can be
# judged on its own is judged. A check stops early only when what follows
# cannot be judged without a missing or bad value, and then it skips only
# that dependent part, in silence: the missing or bad value has its line.
# A missing field reads as None from _Checker.field(). The dependent parts:
# a layer's fields need its kind; the whole model needs the format; bounds
# from rate_hz, a sound's len, a tone's len and pitch_mhz, or a noise env
# wait for those; the render budget waits for a model with no fault.
class _Checker:
    def __init__(self):
        self.faults = []

    def fault(self, where, text):
        self.faults.append(f"{where or 'model'}: {text}")

    def keys(self, obj, where, section):
        """Report unknown and missing fields. False only if obj is not an
        object: then no field of it can be read."""
        if not isinstance(obj, dict):
            self.fault(where, "must be an object")
            return False
        for k in obj:
            if k not in FIELDS[section]:
                self.fault(f"{where}.{k}" if where else k, "unknown field")
        for k in sorted(REQUIRED[section]):
            if k not in obj:
                self.fault(where, f"missing field {k!r}")
        return True

    def int(self, v, where, lo, hi):
        if not _is_int(v):
            self.fault(where, "must be a whole number")
            return False
        if not lo <= v <= hi:
            self.fault(where, f"must be {lo}, not {v}" if lo == hi
                       else f"must be {lo} to {hi}, not {v}")
            return False
        return True

    def field(self, obj, section, key, where, lo=None, hi=None, items=None):
        """The value of obj[key] if its type and range are right, else None.

        The type comes from FIELDS. lo and hi bound an int, or the length of
        a list. items gives the two ranges of the numbers in each pair.
        A fault names the field. A bound that depends on other fields is
        checked by the caller after this.
        """
        if key not in obj:
            return None             # keys() gave the missing line, if required
        kind = FIELDS[section][key]
        v = obj[key]
        w = f"{where}.{key}" if where else key
        if kind == "int":
            return v if self.int(v, w, lo, hi) else None
        if kind == "bool":
            if isinstance(v, bool):
                return v
            self.fault(w, "must be true or false")
        elif kind == "kind":
            if v == section:
                return v
            self.fault(w, 'must be "tone" or "noise"')
        elif kind == "object":
            if isinstance(v, dict) and v:
                return v
            self.fault(w, "must be an object with at least one sound")
        elif kind == "list":
            if isinstance(v, list) and lo <= len(v) <= hi:
                return v
            self.fault(w, f"must be a list of {lo} to {hi} layers")
        elif kind == "pair":
            if not isinstance(v, list) or len(v) != 2:
                self.fault(w, "must be [from, to] in millihertz")
                return None
            ok = [self.int(x, f"{w}[{i}]", lo, hi) for i, x in enumerate(v)]
            return v if all(ok) else None
        else:
            return self._pairs(v, w, lo, hi, items)
        return None

    def _pairs(self, v, w, lo, hi, items):
        if not isinstance(v, list):
            self.fault(w, "must be a list")
            return None
        # The length and each item are judged on their own.
        ok = lo <= len(v) <= hi
        if not ok:
            self.fault(w, f"must hold {lo} to {hi} points, not {len(v)}")
        for i, p in enumerate(v):
            if not isinstance(p, list) or len(p) != 2:
                self.fault(f"{w}[{i}]", "must be a pair of whole numbers")
                ok = False
                continue
            for j in (0, 1):
                if not self.int(p[j], f"{w}[{i}][{j}]", *items[j]):
                    ok = False
        return v if ok else None


def sine_bound(n, from_mhz, to_mhz, multiple, rate):
    """An upper bound of every sine argument of a tone partial, phase * mult.

    Each partial on its own: a lower multiple can sound while a higher one
    is silent, so the largest multiple bounds nothing.

    At sample i, phase is the sum of 2 pi f_j / rate for j < i, and the
    partial calls sin(phase * mult) only while f_i * mult < rate / 2.
    - Rising or held pitch: every f_j <= f_i, so phase * mult <=
      2 pi i f_i mult / rate < pi i < pi n. A partial that goes silent part
      way never calls sin() again. pi * 262144 is far under the limit.
    - Falling pitch: the partial may start silent and sound from some sample
      on, so bound the whole sum. The phase after n samples is at most
      2 pi / rate * (f0 + n * (f0 - f1) / ln(f0 / f1)): a falling geometric
      sum is at most its first term plus its integral. A partial still above
      half the rate at f1, with 1 % to spare, never calls sin().
    The 1 % margins cover the loop move of 0.05 % and the rounding of the
    ratio, of f and of the phase sum, each under 1e-10 relative.
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


def _check_layer(c, layer, li, slen, rate, where):
    w = f"{where}.layers[{li}]"
    if not isinstance(layer, dict):
        c.fault(w, "must be an object")
        return
    kind = layer.get("kind")
    if kind not in ("tone", "noise"):
        # Which fields a layer has, and their bounds, depend on its kind.
        c.fault(f"{w}.kind", 'must be "tone" or "noise"')
        return
    c.keys(layer, w, kind)
    # rate is None when rate_hz is bad: bounds from it are skipped.
    top = 48000 if rate is None else rate
    mdb = (MDB_MIN, MDB_MAX)
    c.field(layer, kind, "hits", w, 1, MAX_HITS, ((0, slen - 1), mdb))
    c.field(layer, kind, "level_mdb", w, *mdb)
    env = None
    if "env" in layer:
        env = c.field(layer, kind, "env", w, 1 if kind == "tone" else 2, MAX_ENV,
                      ((0, MAX_US), mdb))
        for i in range(1, len(env) if env else 0):
            if env[i][0] < env[i - 1][0]:
                c.fault(f"{w}.env[{i}][0]", "must not come before the point above it")
                env = None
                break
    if kind == "tone":
        n = c.field(layer, kind, "len", w, 1, MAX_LEN)
        pitch = c.field(layer, kind, "pitch_mhz", w, 1, top * 500)
        partials = c.field(layer, kind, "partials", w, 1, MAX_PARTIALS, ((1, 1000000), mdb))
        if None not in (n, pitch, partials, rate):
            for i, (mult, _) in enumerate(partials):
                bound = sine_bound(n, pitch[0], pitch[1], mult, rate)
                if bound > SINE_LIMIT:
                    c.fault(f"{w}.partials[{i}]", f"the sine of multiple {mult} can reach"
                            f" {bound:.0f} radians, past the limit {SINE_LIMIT:.0f}."
                            " Shorten the glide or lower the multiple.")
        for key in ("attack_us", "release_us"):
            if key in layer:
                v = c.field(layer, kind, key, w, 0, MAX_US)
                if None not in (v, n, rate) and v * rate > n * 1000000:
                    c.fault(f"{w}.{key}", "must fit inside the tone's len")
    else:
        c.field(layer, kind, "seed", w, 0, MASK)
        bands = c.field(layer, kind, "bands", w, 2, MAX_BANDS, ((0, top // 2), mdb))
        for i in range(1, len(bands) if bands else 0):
            if bands[i][0] <= bands[i - 1][0]:
                c.fault(f"{w}.bands[{i}][0]", "must be above the point above it")
        if "repeat" in layer:
            c.field(layer, kind, "repeat", w)
        fold_hi = MAX_LEN // 2
        if env is not None and rate is not None:
            n = noise_len(layer, rate)
            if n < 1 or n > MAX_LEN:
                c.fault(f"{w}.env", f"must span 1 to {MAX_LEN} samples, not {n}")
            else:
                fold_hi = n // 2
        if "fold" in layer:
            c.field(layer, kind, "fold", w, 0, fold_hi)


def check_model(model):
    """Faults of a parsed model, as a list of messages."""
    c = _Checker()
    if not c.keys(model, "", "top"):
        return c.faults
    # Another format, or none, gives the other fields another meaning.
    if c.field(model, "top", "format", "", FORMAT, FORMAT) is None:
        return c.faults
    rate = c.field(model, "top", "rate_hz", "", 8000, 48000)
    ceiling = c.field(model, "top", "ceiling_steps", "", 1, 127)
    sounds = c.field(model, "top", "sounds", "")
    names = set()
    for name, sound in (sounds or {}).items():
        where = f"sound {name!r}"
        if not NAME.match(name):
            c.fault(where, "the name must be a to z, 0 to 9 and _, start with a letter,"
                           " at most 32 characters")
        if not c.keys(sound, where, "sound"):
            continue
        n = c.field(sound, "sound", "len", where, 1, MAX_LEN)
        c.field(sound, "sound", "peak_steps", where, 1, 127 if ceiling is None else ceiling)
        if "loop" in sound:
            c.field(sound, "sound", "loop", where)
        tabs = [name]
        if "split" in sound:
            hi = MAX_LEN - 1 if n is None else n - 1
            if c.field(sound, "sound", "split", where, 1, hi) is not None and n is not None:
                tabs = [name + "_1", name + "_2"]
        for t in tabs:
            if t in names:
                c.fault(where, f"the table name {t!r} is taken twice")
            names.add(t)
        layers = c.field(sound, "sound", "layers", where, 1, MAX_LAYERS)
        # Each layer is judged even when the list has a bad length.
        if layers is None and isinstance(sound.get("layers"), list):
            layers = sound["layers"]
        for li, layer in enumerate(layers or []):
            _check_layer(c, layer, li, MAX_LEN if n is None else n, rate, where)
    if not c.faults:
        cost = render_cost(model)
        if cost > MAX_COST:
            c.fault("", f"the render costs {cost} units, over the budget of {MAX_COST}."
                    " Shorten the sounds, or cut hits, layers or partials.")
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


def _no_float(text):
    raise ValueError(f"{text} is not a whole number: write JSON integers only")


def _whole(text):
    if len(text.lstrip("-")) > MAX_DIGITS:
        raise ValueError(f"a number of {len(text.lstrip('-'))} digits is too large"
                         " for any field")
    return int(text)


def parse_model(text):
    """A model from JSON text, checked. Raises ModelError.

    Every number must be a JSON integer: a decimal point or an exponent,
    as in 1.0 or 1e0, is refused while parsing, before any value compares
    equal to an integer. NaN, Infinity and a field twice are refused too.
    """
    try:
        model = json.loads(text, object_pairs_hook=_no_duplicates,
                           parse_constant=_no_constant, parse_float=_no_float,
                           parse_int=_whole)
    except (ValueError, RecursionError) as e:
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
    """The include guard of a header name. It starts with SFX_GUARD_, so it
    is never a name C reserves, such as E plus a capital for <errno.h>,
    whatever the file is called.

    The map is one to one on the names H_FILE takes. Such a name is a stem
    of [a-z0-9_] and then .h. The stem goes to capitals letter by letter,
    and two different stems give two different capital stems, because the
    stem has no capital to meet. The one . becomes _, but every guard ends
    in _H after its stem, so the stem is the guard minus SFX_GUARD_ and _H.
    Two different names therefore give two different guards."""
    return "SFX_GUARD_" + re.sub(r"[^A-Z0-9]", "_", h_name.upper())


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
    """One line for each identifier written twice. The marker rule of
    PREFIX keeps every name clear of C's own names, so only the tool's own
    names can clash: two sounds that give one table name, or a split's _1
    and _2 against another sound. The guard has its own start, SFX_GUARD_."""
    faults, seen = [], {}
    for ident, where in names:
        if ident in seen:
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


def _side(path, kind, made):
    """A new side file next to path, such as .name.<pid>.tmp. Its name goes
    into made as soon as the file exists, before a byte is written. Returns
    an open file."""
    folder, base = os.path.split(path)
    # path is a resolved path from the Plan, not an argument.
    side = os.path.join(folder, f".{base}.{os.getpid()}.{kind}")
    fd = os.open(side, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o666)
    made.append(side)
    try:
        return side, os.fdopen(fd, "wb")
    except BaseException:
        os.close(fd)
        raise


def _backup(path, made):
    """Keep the old content of path under .name.<pid>.bak: a hard link if
    the file system allows it, else a copy. The name goes into made as soon
    as the file exists."""
    folder, base = os.path.split(path)
    # path is a resolved path from the Plan, not an argument.
    bak = os.path.join(folder, f".{base}.{os.getpid()}.bak")
    try:
        os.link(path, bak, follow_symlinks=False)
        made.append(bak)
        return bak
    except OSError:
        pass
    bak, f = _side(path, "bak", made)
    with f, open(path, "rb") as old:
        f.write(old.read())
    return bak


def _quiet(f, *a):
    try:
        f(*a)
    except OSError:
        pass


def publish(items, new_dirs=()):
    """Publish every (path, bytes) of a run as one. First each folder of
    new_dirs is made, parents first; one that exists by then is used and
    not made. Each item goes to a temporary file in its folder. Then each
    existing path gets a backup, and the temporary files are renamed in
    order, so the caller puts the header last.

    Every side file and folder is recorded the moment it exists, before
    anything goes into it. On any failure, every path this call renamed
    gets its backup back, or is removed if it had none, every recorded side
    file is removed, every folder this call made is removed if empty, and
    the error is raised. On success the backups are removed. A path is
    never left half written."""
    dirs, sides, backups, done = [], [], {}, []
    try:
        for d in new_dirs:
            try:
                os.mkdir(d)
            except FileExistsError:
                if not os.path.isdir(d):
                    raise
                continue
            dirs.append(d)
        temps = []
        for path, data in items:
            tmp, f = _side(path, "tmp", sides)
            temps.append((tmp, path))
            with f:
                f.write(data)
        for _, path in temps:
            if os.path.lexists(path):
                backups[path] = _backup(path, sides)
        for tmp, path in temps:
            os.replace(tmp, path)
            done.append(path)
            sides.remove(tmp)
    except BaseException:
        for path in reversed(done):
            if path in backups:
                # A backup that cannot go back stays: it holds the old content.
                bak = backups[path]
                sides.remove(bak)
                _quiet(os.replace, bak, path)
            else:
                _quiet(os.unlink, path)
        for side in sides:
            _quiet(os.unlink, side)
        for d in reversed(dirs):
            _quiet(os.rmdir, d)
        raise
    for bak in backups.values():
        _quiet(os.unlink, bak)


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

# The file names of --c and --h, as written. Each goes into the generated C:
# the header's name into #include "..." and the include guard. A lower-case
# letter, then lower-case letters, digits and _, then one . and the
# extension, so the name needs no escape in a C string, is ASCII, and maps
# to its guard one to one: see header_guard. Each name holds sfx, which no
# standard or system header's name does, so a generated header never takes
# the place of one on the include path.
C_FILE = re.compile(r"[a-z][a-z0-9_]*\.c")
H_FILE = re.compile(r"[a-z][a-z0-9_]*\.h")


def file_name_faults(c_path, h_path):
    """One line for each bad file name of --c and --h. Empty when both are
    good."""
    faults = []
    for opt, path, rule, ext in (("--c", c_path, C_FILE, ".c"), ("--h", h_path, H_FILE, ".h")):
        name = os.path.basename(path)
        if not rule.fullmatch(name) or "sfx" not in name:
            faults.append(f"{opt} {name!r}: the file name must be a lower-case letter, then"
                          f" lower-case letters, digits or _, hold sfx, and end in {ext}")
    return faults


def _same_file(a, b):
    """True if resolved paths a and b name the same file: the same inode
    where both exist, else the same path, compared with case folded too.
    The case rule is strict on every system, so a run that works on one
    file system works on all."""
    try:
        if os.path.samefile(a, b):
            return True
    except OSError:
        pass
    return a == b or a.casefold() == b.casefold()


def _under(real, top):
    """True if the resolved path real lies below the folder or file top, by
    the same file rule."""
    here = os.path.dirname(real)
    while True:
        if _same_file(here, top):
            return True
        up = os.path.dirname(here)
        if up == here:
            return False
        here = up


# One path of a run: its label, its spelling as written, and where it
# resolved. Messages show the spelling; everything else uses real.
Node = collections.namedtuple("Node", "label path real")


class Plan:
    """Every file and folder of one run, each resolved exactly once.

    This class is the only place that resolves a path or joins one to an
    argument; a test holds the tool to it. Loading the model, making
    folders, temporary and backup names, renames, both checks and messages
    all take their path from here, so no later step can follow a symbolic
    link the plan did not see. The plan is a set of nodes, files and
    folders. Files: the model and each output. Folders: --dump, --wav and
    the folder of each file. Paths compare by _same_file.
    """

    def __init__(self, model, folders=()):
        self._shown = {}
        self.model = self._resolve("MODEL", model)
        self.files = [self.model]
        self.folders = {lb: self._resolve(lb, d) for lb, d in folders if d}
        self.new = []

    def _resolve(self, label, path, real=None):
        node = Node(label, path, os.path.realpath(path if real is None else real))
        self._shown[node.real] = path
        return node

    def file(self, label, path):
        """Add an output file given as written."""
        node = self._resolve(label, path)
        self.files.append(node)
        return node

    def file_in(self, label, name):
        """Add the output file name in the folder of label. name has no
        separator. The file is resolved in the folder's resolved path."""
        d = self.folders[label]
        node = self._resolve(label, os.path.join(d.path, name),
                             os.path.join(d.real, name))
        self.files.append(node)
        return node

    def shown(self, real):
        """The spelling of a resolved path, for a message."""
        return self._shown.get(real, real)

    def fault(self):
        """Check the whole plan before anything is written. One line, or
        None if the plan is good; then self.new holds the resolved folders
        to make, parents first. Refused: two files at one path, which covers
        an output equal to the model; one path as a file and a folder; a
        node under a file; a file that is a folder on disk; a folder that is
        a file on disk; a missing folder that is neither made by the run nor
        in a folder that is; one new folder written two ways that differ
        only by case."""
        files, folders = self.files, list(self.folders.values())
        dirs, seen = [], set()
        for node in folders + [Node(f"the folder of {lb}", os.path.dirname(p) or ".",
                                    os.path.dirname(r)) for lb, p, r in files]:
            if node.real not in seen:
                seen.add(node.real)
                dirs.append(node)
        for i, (la, pa, ra) in enumerate(files):
            for lb, pb, rb in files[i + 1:]:
                if _same_file(ra, rb):
                    return f"{la} {pa!r} and {lb} {pb!r} are the same file"
        for lf, pf, rf in files:
            for ld, pd, rd in dirs:
                if _same_file(rf, rd):
                    return f"{lf} {pf!r} and {ld} {pd!r} are one path, a file and a folder"
            for ln, pn, rn in files + dirs:
                if _under(rn, rf):
                    return f"{ln} {pn!r} lies under {lf} {pf!r}, a file"
            if os.path.isdir(rf):
                return f"{lf} {pf!r} is a directory"
        new = [d for d in folders if not os.path.isdir(d.real)]
        for i, (la, pa, ra) in enumerate(new):
            for lb, pb, rb in new[i + 1:]:
                if _same_file(ra, rb) and ra != rb:
                    return (f"{la} {pa!r} and {lb} {pb!r} differ only by case:"
                            " write the folder the same way twice")
        for ld, pd, rd in dirs:
            if os.path.lexists(rd) and not os.path.isdir(rd):
                return f"{ld} {pd!r} is not a directory"
            # A folder the run makes needs its parent; any other must exist.
            if not any(_same_file(rd, d.real) for d in new):
                if not os.path.isdir(rd):
                    return f"{ld} {pd!r} does not exist"
                continue
            parent = os.path.dirname(rd)
            if not os.path.isdir(parent) and not any(_same_file(parent, d.real) for d in new):
                return f"{ld} {pd!r}: the folder {parent!r} does not exist"
        self.new = sorted({d.real for d in new}, key=len)
        return None


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


def _check(args, plan):
    # The paths of a write of this command line, from the plan.
    h, c = plan.file("--h", args.h), plan.file("--c", args.c)
    # A check answers for the same command line as a write, so it refuses
    # the paths a write would refuse.
    fault = plan.fault()
    if fault:
        print(f"sfx_table.py: error: {fault}", file=sys.stderr)
        return EX_USAGE
    try:
        h_raw = _read_bytes(h.real)
        c_raw = _read_bytes(c.real)
        sha = hashlib.sha256(_read_bytes(plan.model.real)).hexdigest()
    except OSError as e:
        print(f"sfx_table.py: {plan.shown(e.filename)}: {e.strerror}", file=sys.stderr)
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
        model, _ = load_model(plan.model.real)
        built = build(model)
        source = os.path.basename(args.model)
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
    p.add_argument("--prefix", metavar="NAME_sfx_")
    p.add_argument("--check", action="store_true",
                   help="compare the recorded SHA-256 and revision, write nothing")
    p.add_argument("--full", action="store_true", help="with --check: render and compare bytes")
    p.add_argument("--dump", metavar="DIR", help="write each table as a raw .u8 file")
    p.add_argument("--wav", metavar="DIR", help="write each sound as an 8-bit mono WAV")
    args = p.parse_args(argv)
    try:
        return _main(p, args)
    except Exception as e:
        # The last backstop: one line, never a traceback.
        text = " ".join(str(e).split())
        print(f"sfx_table.py: {args.model}: internal error: {type(e).__name__}: {text}",
              file=sys.stderr)
        return EX_SOFTWARE


def _main(p, args):
    table = (args.c, args.h, args.prefix)
    if any(v is not None for v in table) and None in table:
        p.error("--c, --h and --prefix go together")
    if args.prefix is not None and not PREFIX.match(args.prefix):
        p.error(f"--prefix {args.prefix!r} must match ([a-z][a-z0-9]*_)*sfx_,"
                " at most 32 characters, such as sfx_ or uos_sfx_")
    if args.c is not None and args.h is not None:
        for f in file_name_faults(args.c, args.h):
            p.error(f)
    # The name goes into a C comment as written. Letters, digits, '_', '.'
    # and '-' cannot end the comment, start a trigraph or break a line.
    if args.c is not None and not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*",
                                               os.path.basename(args.model)):
        p.error("MODEL must have a file name of letters, digits, '_', '.' and '-',"
                " starting with a letter or a digit: it goes into the comment of"
                " each generated file")
    if args.full and not args.check:
        p.error("--full needs --check")
    if args.check and (args.c is None or args.dump or args.wav):
        p.error("--check needs --c, --h and --prefix, and goes alone")
    if args.c is None and not args.dump and not args.wav:
        p.error("nothing to do: give --c, --h and --prefix, --dump or --wav")

    plan = Plan(args.model, [("--dump", args.dump), ("--wav", args.wav)])
    try:
        if args.check:
            return _check(args, plan)
        model, sha = load_model(plan.model.real)
        built = build(model)
    except ModelError as e:
        for f in e.faults:
            print(f"sfx_table.py: {args.model}: {f}", file=sys.stderr)
        return EX_MODEL
    except OSError as e:
        print(f"sfx_table.py: {args.model}: {e.strerror}", file=sys.stderr)
        return EX_STALE

    items = []
    if args.c is not None:
        h_name = os.path.basename(args.h)
        source = os.path.basename(args.model)
        h_text = c_header(model, built, args.prefix, source=source, sha=sha, h_name=h_name)
        faults = c_name_faults(c_names(model, built, args.prefix, h_name))
        if faults:
            p.print_usage(sys.stderr)
            for f in faults:
                print(f"sfx_table.py: error: {f}", file=sys.stderr)
            return EX_USAGE
        items.append((plan.file("--c", args.c), c_source(model, built, args.prefix, source,
                                                         sha, h_name).encode("ascii")))
    # --dump and --wav name each file after a table or a sound. A sound
    # name matches NAME, so a file name is [a-z][a-z0-9_]* with .u8 or
    # .wav: no path separator, no dot file, ASCII. A test checks it.
    if args.dump:
        items += [(plan.file_in("--dump", t + ".u8"), bytes(part))
                  for name, data in built.items()
                  for t, part in tables(model, name, data)]
    if args.wav:
        for name, data in built.items():
            times = 3 if model["sounds"][name].get("loop") else 1
            items.append((plan.file_in("--wav", name + ".wav"),
                          wav_bytes(data * times, model["rate_hz"])))
    if args.c is not None:
        # The header last: it carries the hash that --check reads.
        items.append((plan.file("--h", args.h), h_text.encode("ascii")))
    fault = plan.fault()
    if fault:
        p.print_usage(sys.stderr)
        print(f"sfx_table.py: error: {fault}", file=sys.stderr)
        return EX_USAGE
    # Publish writes the paths the plan resolved, never resolving again.
    try:
        publish([(node.real, data) for node, data in items], plan.new)
    except OSError as e:
        print(f"sfx_table.py: {plan.shown(e.filename)}: {e.strerror}", file=sys.stderr)
        return EX_STALE
    return 0


if __name__ == "__main__":
    sys.exit(main())
