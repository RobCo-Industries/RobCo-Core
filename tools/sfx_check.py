#!/usr/bin/env python3
"""Check that dumped sound effect tables sound like their model.

Usage: sfx_check.py MODEL DIR

MODEL is a sound effect model, format 1, as in docs/spec/SFX_MODEL.md.
DIR holds the raw `<table>.u8` files that `sfx_table.py MODEL --dump DIR`
writes. For each sound the tool measures the band power and the frame level
of the table, predicts both from the model alone, and compares them in dB.

This tool shares no code with sfx_table.py. It holds its own small copy of
the pure math it needs and uses Python's math module: it judges by
tolerance, not by bytes.

Exit codes: 0 all sounds pass, 1 a sound fails, 2 the model is not valid or
a table file is missing or has the wrong length, 64 usage, 70 internal fault.
"""

import cmath
import json
import math
import operator
import os
import sys

EX_OK = 0
EX_FAIL = 1
EX_MODEL = 2
EX_USAGE = 64
EX_SOFTWARE = 70

BAND_HZ = 250.0
FRAME_S = 0.005
FLOOR_DB = 30.0
TOP_HZ = 15000.0
FIR_TAPS = 511
DESIGN_BINS = 1024
FILTER_FFT = 8192
# Rounding each sample to a whole step adds white noise of this power, in
# steps squared, per sample: a uniform error over one step.
ROUND_POWER = 1.0 / 12.0
# The count of independent complex values a judged band or frame of a
# sound with noise must hold, by kind of sound. Each limit sits about five
# standard deviations out, so a table with dozens of bands or hundreds of
# frames does not fail by chance. A 10 dB limit needs 20 real numbers, a
# 6 or 7 dB limit 40, and a 1.5 dB limit about 400.
SPAN = {"noise": {"band": 10.0, "frame": 20.0},
        "loop": {"band": 200.0, "frame": 20.0}}
# Fewer bands or frames than this are not judged.
MIN_COUNT = 3
# Bands are not judged when the table and the model both hold less than
# this share of the energy under 15 kHz.
LOW_SHARE = 0.01
# Bounds of the level of a sound with noise: how far its peak sits above
# the strongest expected rms of one sample, in dB.
LEVEL_MIN_DB = -6.0
LEVEL_MAX_DB = 14.0
# The generator's ranges for the fields this tool reads.
MAX_LEN = 262144
MAX_US = MAX_LEN * 1000000 // 8000
MDB = (-120000, 20000)

# Tolerances in dB, by kind of sound. Each rule names a statistic of the
# absolute errors and its limit.
RULES = {
    "tones": {"band": [("worst", 0.5)], "frame": [("worst", 0.5)]},
    "noise": {"band": [("mean", 3.0), ("worst", 10.0)],
              "frame": [("mean", 2.0), ("worst", 6.0)]},
    "loop": {"band": [("worst", 1.5)],
             "frame": [("median", 1.5), ("worst", 7.0)]},
}


class ModelError(Exception):
    pass


class TableError(Exception):
    pass


# ---- the model --------------------------------------------------------------

def _refuse_constant(name):
    raise ModelError("number %s is not allowed" % name)


def _refuse_float(text):
    raise ModelError("number %s is not an integer" % text)


def _no_duplicates(pairs):
    out = {}
    for k, v in pairs:
        if k in out:
            raise ModelError("field %r appears twice" % k)
        out[k] = v
    return out


def _is_int(v):
    return isinstance(v, int) and not isinstance(v, bool)


def _need_int(obj, key, where, lo, hi, optional=False):
    if key not in obj:
        if optional:
            return
        raise ModelError("%s: missing %s" % (where, key))
    v = obj[key]
    if not _is_int(v) or not lo <= v <= hi:
        raise ModelError("%s: %s must be an integer from %d to %d" % (where, key, lo, hi))


def _need_pairs(obj, key, where, lo, hi, items, optional=False):
    if key not in obj:
        if optional:
            return
        raise ModelError("%s: missing %s" % (where, key))
    v = obj[key]
    if (not isinstance(v, list) or not lo <= len(v) <= hi
            or not all(isinstance(p, list) and len(p) == 2
                       and _is_int(p[0]) and _is_int(p[1]) for p in v)):
        raise ModelError("%s: %s must be %d to %d integer pairs" % (where, key, lo, hi))
    for p in v:
        for x, (a, b) in zip(p, items):
            if not a <= x <= b:
                raise ModelError("%s: %s holds %d, outside %d to %d" % (where, key, x, a, b))


def load_model(path):
    """Read and check the parts of a model this tool needs."""
    try:
        with open(path, "rb") as f:
            raw = f.read()
    except OSError as e:
        raise ModelError("cannot read %s: %s" % (path, e.strerror))
    try:
        model = json.loads(raw.decode("utf-8"), parse_float=_refuse_float,
                           parse_constant=_refuse_constant,
                           object_pairs_hook=_no_duplicates)
    except (UnicodeDecodeError, ValueError) as e:
        raise ModelError("%s is not valid JSON: %s" % (path, e))
    if not isinstance(model, dict):
        raise ModelError("the model is not a JSON object")
    if model.get("format") != 1 or not _is_int(model.get("format")):
        raise ModelError("format must be 1")
    _need_int(model, "rate_hz", "model", 8000, 48000)
    _need_int(model, "ceiling_steps", "model", 1, 127)
    sounds = model.get("sounds")
    if not isinstance(sounds, dict) or not sounds:
        raise ModelError("model: sounds must be a non-empty object")
    for name, s in sounds.items():
        where = "sound %s" % name
        if not isinstance(s, dict):
            raise ModelError("%s: not an object" % where)
        _need_int(s, "len", where, 1, MAX_LEN)
        _need_int(s, "peak_steps", where, 1, model["ceiling_steps"])
        _need_int(s, "split", where, 1, s["len"] - 1, optional=True)
        if "loop" in s and not isinstance(s["loop"], bool):
            raise ModelError("%s: loop must be true or false" % where)
        layers = s.get("layers")
        if not isinstance(layers, list) or not 1 <= len(layers) <= 16:
            raise ModelError("%s: layers must be 1 to 16 layers" % where)
        for j, layer in enumerate(layers):
            lw = "%s layers[%d]" % (where, j)
            if not isinstance(layer, dict) or layer.get("kind") not in ("tone", "noise"):
                raise ModelError("%s: kind must be tone or noise" % lw)
            _need_pairs(layer, "hits", lw, 1, 64, ((0, s["len"] - 1), MDB))
            _need_int(layer, "level_mdb", lw, *MDB)
            if layer["kind"] == "tone":
                _need_int(layer, "len", lw, 1, MAX_LEN)
                pm = layer.get("pitch_mhz")
                top = model["rate_hz"] * 500
                if (not isinstance(pm, list) or len(pm) != 2
                        or not all(_is_int(p) and 1 <= p <= top for p in pm)):
                    raise ModelError("%s: pitch_mhz must be two integers from 1 to %d" % (lw, top))
                _need_pairs(layer, "partials", lw, 1, 16, ((1, 1000000), MDB))
                for key in ("attack_us", "release_us"):
                    _need_int(layer, key, lw, 0, MAX_US, optional=True)
                    if layer.get(key, 0) * model["rate_hz"] > layer["len"] * 1000000:
                        raise ModelError("%s: %s must fit inside the tone's len" % (lw, key))
                _need_pairs(layer, "env", lw, 1, 64, ((0, MAX_US), MDB), optional=True)
            else:
                _need_pairs(layer, "env", lw, 2, 64, ((0, MAX_US), MDB))
                _need_pairs(layer, "bands", lw, 2, 24, ((0, model["rate_hz"] // 2), MDB))
                _need_int(layer, "fold", lw, 0, MAX_LEN // 2, optional=True)
                if "repeat" in layer and not isinstance(layer["repeat"], bool):
                    raise ModelError("%s: repeat must be true or false" % lw)
                pts = layer["bands"]
                if any(pts[k][0] >= pts[k + 1][0] for k in range(len(pts) - 1)):
                    raise ModelError("%s: bands frequencies must rise" % lw)
            env = layer.get("env", [])
            if any(env[k][0] > env[k + 1][0] for k in range(len(env) - 1)) or \
                    any(t < 0 for t, _ in env):
                raise ModelError("%s: env times must start at 0 or later and not go down" % lw)
            if layer["kind"] == "noise":
                n = ceil_div(env[-1][0] * model["rate_hz"], 1000000)
                if not 1 <= n <= MAX_LEN:
                    raise ModelError("%s: env must span 1 to %d samples" % (lw, MAX_LEN))
                if layer.get("fold", 0) > n // 2:
                    raise ModelError("%s: fold must be %d or less, half the env" % (lw, n // 2))
    return model


# ---- the math ---------------------------------------------------------------

def db_gain(db):
    return 10.0 ** (db / 20.0)


def ceil_div(a, b):
    return -((-a) // b)


def env_gain(points, n, rate, hold_last):
    """The envelope of a hit, as in the spec: straight lines in dB."""
    g = [0.0] * n
    s = [t * rate / 1000000.0 for t, _ in points]
    c = [ceil_div(t * rate, 1000000) for t, _ in points]
    for a in range(len(points) - 1):
        da, db = points[a][1] / 1000.0, points[a + 1][1] / 1000.0
        span = s[a + 1] - s[a]
        for i in range(max(c[a], 0), min(c[a + 1], n)):
            g[i] = db_gain(da + (db - da) * (i - s[a]) / span)
    if hold_last:
        last = db_gain(points[-1][1] / 1000.0)
        for i in range(max(c[-1], 0), n):
            g[i] = last
    return g


def fft(x):
    """Radix-2 FFT of a complex list whose length is a power of 2."""
    n = len(x)
    a = list(x)
    j = 0
    for i in range(1, n):
        bit = n >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j |= bit
        if i < j:
            a[i], a[j] = a[j], a[i]
    size = 2
    while size <= n:
        half = size >> 1
        step = cmath.exp(-2j * math.pi / size)
        tw = [1.0 + 0j] * half
        for k in range(1, half):
            tw[k] = tw[k - 1] * step if k % 64 else cmath.exp(-2j * math.pi * k / size)
        for start in range(0, n, size):
            for k in range(half):
                u = a[start + k]
                v = a[start + k + half] * tw[k]
                a[start + k] = u + v
                a[start + k + half] = u - v
        size <<= 1
    return a


def band_energy(x, rate, nbands):
    """Energy of x in each 250 Hz band, so that all bands sum to sum(x^2)."""
    n = 1
    while n < len(x):
        n <<= 1
    spec = fft([complex(v) for v in x] + [0j] * (n - len(x)))
    bands = [0.0] * nbands
    for k in range(n // 2 + 1):
        w = 1.0 if k == 0 or k == n // 2 else 2.0
        b = min(int(k * rate / n / BAND_HZ), nbands - 1)
        z = spec[k]
        bands[b] += w * (z.real * z.real + z.imag * z.imag) / n
    return bands


def frame_energy(power, rate):
    """Sum of a per-sample power in 5 ms frames. The last frame may be short."""
    step = max(1, int(round(FRAME_S * rate)))
    return [sum(power[i:i + step]) for i in range(0, len(power), step)]


def _add(out, start, one, scale, loop):
    n = len(out)
    for i, v in enumerate(one):
        if v == 0.0:
            continue
        j = start + i
        if j >= n:
            if not loop:
                break
            j %= n
        out[j] += v * scale


# ---- the prediction ---------------------------------------------------------

def tone_hit(layer, sound, rate):
    n = layer["len"]
    f0 = layer["pitch_mhz"][0] / 1000.0
    f1 = layer["pitch_mhz"][1] / 1000.0
    ratio = math.exp(math.log(f1 / f0) / n)
    if sound.get("loop") and n == sound["len"]:
        c, f = 0.0, f0
        for _ in range(n):
            c += f / rate
            f *= ratio
        w = math.floor(c + 0.5)
        if w > 0:
            f0 *= w / c
    att = layer.get("attack_us", 0) * rate / 1000000.0
    rel = layer.get("release_us", 0) * rate / 1000000.0
    env = env_gain(layer["env"], n, rate, True) if "env" in layer else None
    parts = [(m / 10000.0, db_gain((layer["level_mdb"] + lv) / 1000.0))
             for m, lv in layer["partials"]]
    one = [0.0] * n
    phase, f, nyq = 0.0, f0, rate / 2.0
    for i in range(n):
        g = 1.0
        if i < att:
            g = (i + 0.5) / att
        if n - i < rel:
            g = min(g, (n - i - 0.5) / rel)
        if env is not None:
            g *= env[i]
        if g != 0.0:
            v = 0.0
            for mult, amp in parts:
                if f * mult < nyq:
                    v += amp * math.sin(phase * mult)
            one[i] = v * g
        phase += 2.0 * math.pi * f / rate
        f *= ratio
    return one


def fir_taps(bands_pts, rate):
    """The noise filter's 511 taps, scaled to unit power."""
    amp = []
    for k in range(DESIGN_BINS // 2 + 1):
        f = k * rate / DESIGN_BINS
        level = None
        for a in range(len(bands_pts) - 1):
            fa, la = bands_pts[a]
            fb, lb = bands_pts[a + 1]
            if fa <= f <= fb:
                level = (la + (lb - la) * (f - fa) / (fb - fa)) / 1000.0
                break
        amp.append(0.0 if level is None else db_gain(level))
    half = FIR_TAPS // 2
    h = [0.0] * FIR_TAPS
    for m in range(half + 1):
        v = amp[0] + amp[DESIGN_BINS // 2] * math.cos(math.pi * m)
        for k in range(1, DESIGN_BINS // 2):
            if amp[k]:
                v += 2.0 * amp[k] * math.cos(2.0 * math.pi * k * m / DESIGN_BINS)
        v *= 0.5 + 0.5 * math.cos(math.pi * m / (half + 1))
        h[half + m] = h[half - m] = v
    e = math.sqrt(sum(v * v for v in h))
    if e <= 0.0:
        raise ModelError("a noise layer's bands give no level")
    return [v / e for v in h]


def autocorr(x):
    """r[d] = sum x[i] x[i+d] for the lags 0 to 510."""
    return [sum(map(operator.mul, x, x[d:])) if d < len(x) else 0.0
            for d in range(FIR_TAPS)]


def hit_gain(layer, rate):
    """The gain of one hit at each of its samples, without its level: the
    envelope times the fold fade. The generator filters a longer run of
    noise, so every sample of a hit holds the full filter."""
    n = ceil_div(layer["env"][-1][0] * rate, 1000000)
    gain = env_gain(layer["env"], n, rate, False)
    fold = layer.get("fold", 0)
    for i in range(fold):
        u = (i + 0.5) / fold
        gain[i] *= math.sin(math.pi / 2 * u)
        gain[n - fold + i] *= math.cos(math.pi / 2 * u)
    return gain


def table_autocorr(g, first, n, loop):
    """The autocorrelation of a hit's gain as it lands in the table: cut
    after `first` samples, then in a loop each further n samples wrap."""
    r = autocorr(g[:first])
    if loop:
        for a in range(first, len(g), n):
            r = [u + v for u, v in zip(r, autocorr(g[a:a + n]))]
    return r


def onesided(spec, size):
    """|X|^2 of the first half of an FFT, scaled as band_energy() scales it,
    so the values sum to the sum of the squared samples."""
    out = []
    for k in range(size // 2 + 1):
        w = 1.0 if k == 0 or k == size // 2 else 2.0
        z = spec[k]
        out.append(w * (z.real * z.real + z.imag * z.imag) / size)
    return out


def lag_spectrum(c, size):
    """The one-sided spectrum on `size` bins of a power whose
    autocorrelation is c[0], c[1], ...: the FFT of c placed circularly,
    real part, clamped at 0. Scaled as onesided(), so it sums to c[0]."""
    x = [0j] * size
    for d, v in enumerate(c):
        x[d % size] += v
        if d:
            x[-d % size] += v
    spec = fft(x)
    out = []
    for k in range(size // 2 + 1):
        w = 1.0 if k == 0 or k == size // 2 else 2.0
        out.append(w * max(spec[k].real, 0.0) / size)
    return out


def comb(hits, size):
    """|sum a_k exp(-2 pi i j s_k / size)|^2 for j from 0 to size / 2."""
    out = [0.0] * (size // 2 + 1)
    for j in range(len(out)):
        z = 0j
        for s, a in hits:
            z += a * cmath.exp(-2j * math.pi * (j * s % size) / size)
        out[j] = z.real * z.real + z.imag * z.imag
    return out


def noise_layer(layer, sound, rate, size):
    """The expected one-sided spectrum of a noise layer on `size` bins, and
    its expected power at each sample of the sound."""
    h = fir_taps(layer["bands"], rate)
    rh = autocorr(h)
    g = hit_gain(layer, rate)
    n, loop = sound["len"], sound.get("loop", False)
    repeat = layer.get("repeat", False)
    hits = [(s, db_gain((layer["level_mdb"] + lv) / 1000.0)) for s, lv in layer["hits"]]
    if repeat:
        # Every hit replays one waveform: one hit's spectrum times the comb
        # of the starts.
        first = n if loop else max(min(len(g), n - s) for s, _ in hits)
        rg = table_autocorr(g, first, n, loop)
        one = lag_spectrum([u * v for u, v in zip(rh, rg)], size)
        spectrum = [u * v for u, v in zip(one, comb(hits, size))]
    else:
        # Unrelated hits add as powers. Sum their lags, then one FFT.
        c = [0.0] * FIR_TAPS
        cache = {}
        for s, a in hits:
            first = n - s if loop else min(len(g), n - s)
            if first not in cache:
                cache[first] = table_autocorr(g, first, n, loop)
            c = [u + a * a * v * w for u, v, w in zip(c, rh, cache[first])]
        spectrum = lag_spectrum(c, size)
    return spectrum, layer_power(g, rh, hits, n, loop, repeat)


def layer_power(g, rh, hits, n, loop, repeat):
    """Expected power at each sample: each hit's own power, plus with
    repeat the cross terms of hits whose noise sits at most 510 samples
    apart. Hit k's sample i and hit l's sample i - d land on one table
    sample when d is the start difference, modulo n in a loop."""
    power = [0.0] * n
    top = FIR_TAPS - 1
    pairs = ([(k, l) for k in range(len(hits)) for l in range(len(hits))]
             if repeat else [(k, k) for k in range(len(hits))])
    size = len(g)
    for k, l in pairs:
        sk, ak = hits[k]
        sl, al = hits[l]
        base = sl - sk
        if loop:
            lags = [d for d in range(base % n - (top // n + 1) * n, top + 1, n)
                    if -top <= d <= top]
        else:
            lags = [base] if -top <= base <= top else []
        for d in lags:
            lo, hi = max(0, d), min(size, size + d)
            if lo >= hi:
                continue
            coef = ak * al * rh[abs(d)]
            prod = list(map(operator.mul, g[lo:hi], g[lo - d:hi - d]))
            _add(power, sk + lo, prod, coef, loop)
    return power


def render_tones(sound, rate):
    """The sum of every tone hit of a sound, before scaling."""
    loop = sound.get("loop", False)
    tone = [0.0] * sound["len"]
    for layer in sound["layers"]:
        if layer["kind"] == "tone":
            one = tone_hit(layer, sound, rate)
            for start, level in layer["hits"]:
                _add(tone, start, one, db_gain(level / 1000.0), loop)
    return tone


def round_half_away(v):
    return math.floor(abs(v) + 0.5) * (1 if v >= 0 else -1)


def rounded_tones(sound, rate):
    """The table of a tones-only sound as whole steps: the render scaled so
    its loudest sample sits at peak_steps, each sample rounded. Rounding
    follows the waveform here, so it is not white noise."""
    tone = render_tones(sound, rate)
    top = max(abs(v) for v in tone)
    if top <= 0.0:
        raise ModelError("a tones-only sound renders to silence")
    k = sound["peak_steps"] / top
    return [float(round_half_away(v * k)) for v in tone]


def predict(sound, rate, size):
    """The expected one-sided spectrum of a sound with noise on `size`
    bins, the expected power of each sample, and the noise part of both,
    before scaling."""
    tone = render_tones(sound, rate)
    spectrum = [0.0] * (size // 2 + 1)
    if any(layer["kind"] == "tone" for layer in sound["layers"]):
        spectrum = onesided(fft([complex(v) for v in tone] + [0j] * (size - len(tone))), size)
    noise_spec = [0.0] * len(spectrum)
    noise_pow = [0.0] * sound["len"]
    for layer in sound["layers"]:
        if layer["kind"] == "noise":
            spec, power = noise_layer(layer, sound, rate, size)
            noise_spec = [u + v for u, v in zip(noise_spec, spec)]
            noise_pow = [u + v for u, v in zip(noise_pow, power)]
    spectrum = [u + v for u, v in zip(spectrum, noise_spec)]
    sample = [t * t + p for t, p in zip(tone, noise_pow)]
    return spectrum, sample, noise_spec, noise_pow


def widths(noise_spec, noise_pow, rate, size, kind):
    """The band width in Hz and the frame length in samples for a sound
    with noise of this kind, from its effective duration and bandwidth."""
    span = SPAN[kind]
    sp, sp2 = sum(noise_pow), sum(p * p for p in noise_pow)
    ss, ss2 = sum(noise_spec), sum(v * v for v in noise_spec)
    dur = sp * sp / sp2 / rate if sp2 > 0.0 else 0.0
    bw = ss * ss / ss2 * rate / size if ss2 > 0.0 else 0.0
    band = max(BAND_HZ, span["band"] / dur) if dur > 0.0 else float("inf")
    frame = max(FRAME_S, span["frame"] / bw) if bw > 0.0 else float("inf")
    step = max(1, int(round(min(frame * rate, 1e12))))
    return band, step, dur, bw


def counts(sums, squares, scale):
    """The independent values of each band or frame: `scale` times the
    effective number of its bins or samples."""
    return [scale * u * u / v if v > 0.0 else 0.0 for u, v in zip(sums, squares)]


def enough(n, noise, whole, need):
    """Whether each value holds enough independent values to judge. Tones
    are fixed, so a value with share `q` of noise counts n / (q (2 - q));
    a value with no noise is always judged."""
    out = []
    for c, u, w in zip(n, noise, whole):
        q = min(1.0, u / w) if w > 0.0 else 0.0
        out.append(q <= 0.0 or c / (q * (2.0 - q)) >= need)
    return out


def to_bands(spec, rate, size, width):
    """Sum a one-sided spectrum in bands of `width` Hz from 0 up to 15 kHz,
    without a last partial band, and the part under 15 kHz."""
    top = min(TOP_HZ, rate / 2.0)
    nb = int(top // width) if width < float("inf") else 0
    bands = [0.0] * nb
    low = 0.0
    for k, v in enumerate(spec):
        f = k * rate / size
        if f < TOP_HZ or rate / 2.0 <= TOP_HZ:
            low += v
        b = int(f // width) if width < float("inf") else nb
        if b < nb:
            bands[b] += v
    return bands, low


def to_frames(power, step):
    """Sum a per-sample power in frames of `step` samples from sample 0. A
    last frame shorter than half a step joins the one before it."""
    out = [sum(power[i:i + step]) for i in range(0, len(power), step)]
    rest = len(power) % step
    if len(out) > 1 and rest and rest < step / 2.0:
        last = out.pop()
        out[-1] += last
    return out


# ---- the comparison ---------------------------------------------------------

def kind_of(sound):
    if sound.get("loop"):
        return "loop"
    if any(layer["kind"] == "noise" for layer in sound["layers"]):
        return "noise"
    return "tones"


def read_table(name, sound, folder):
    names = [name] if "split" not in sound else [name + "_1", name + "_2"]
    data = b""
    for t in names:
        path = os.path.join(folder, t + ".u8")
        try:
            with open(path, "rb") as f:
                data += f.read()
        except OSError as e:
            raise TableError("cannot read %s: %s" % (path, e.strerror))
    if "split" in sound and os.path.getsize(os.path.join(folder, name + "_1.u8")) != sound["split"]:
        raise TableError("%s_1.u8 is not %d bytes" % (name, sound["split"]))
    if len(data) != sound["len"]:
        raise TableError("%s holds %d samples, the model says %d"
                         % (name, len(data), sound["len"]))
    return data


def errors_db(meas, pred, gain, keep=None):
    """dB errors over the kept entries within FLOOR_DB of the strongest
    measured."""
    top = max(meas)
    if top <= 0.0:
        return []
    if keep is None:
        keep = [True] * len(meas)
    out = []
    for m, p, k in zip(meas, pred, keep):
        if not k or m <= 0.0 or 10.0 * math.log10(top / m) > FLOOR_DB:
            continue
        p *= gain
        out.append(float("inf") if p <= 0.0 else abs(10.0 * math.log10(m / p)))
    return out


def stats(errs):
    if not errs:
        return {"mean": 0.0, "median": 0.0, "worst": 0.0}
    s = sorted(errs)
    k = len(s)
    med = s[k // 2] if k % 2 else (s[k // 2 - 1] + s[k // 2]) / 2.0
    return {"mean": sum(s) / k, "median": med, "worst": s[-1]}


def _gain(meas, floor, pred):
    """The gain that matches the model part of a measured energy, or None
    when the measure does not rise above the rounding floor."""
    if pred > 0.0 and meas > floor:
        return (meas - floor) / pred
    return None


def check_sound(model, name, folder):
    """Return the result line of one sound and whether it passes."""
    sound = model["sounds"][name]
    rate = model["rate_hz"]
    data = read_table(name, sound, folder)
    kind = kind_of(sound)
    faults = []
    notes = []
    peak = max(abs(b - 128) for b in data)
    if peak != sound["peak_steps"]:
        faults.append("peak is %d steps, the model says %d" % (peak, sound["peak_steps"]))
    x = [b - 128.0 for b in data]
    has_noise = any(layer["kind"] == "noise" for layer in sound["layers"])
    if has_noise:
        size = 1
        while size < len(x):
            size <<= 1
        mspec = onesided(fft([complex(v) for v in x] + [0j] * (size - len(x))), size)
        pspec, ps, nspec, npow = predict(sound, rate, size)
        width, step, dur, bw = widths(nspec, npow, rate, size, kind)
        mb, m_low = to_bands(mspec, rate, size, width)
        pb, p_low_model = to_bands(pspec, rate, size, width)
        mf = to_frames([v * v for v in x], step)
        pf = to_frames(ps, step)
        nb_spec, _ = to_bands(nspec, rate, size, width)
        nb_sq, _ = to_bands([v * v for v in nspec], rate, size, width)
        nf_pow = to_frames(npow, step)
        nf_sq = to_frames([v * v for v in npow], step)
        keep_b = enough(counts(nb_spec, nb_sq, dur * rate / size), nb_spec, pb,
                        SPAN[kind]["band"])
        keep_f = enough(counts(nf_pow, nf_sq, bw / rate), nf_pow, pf,
                        SPAN[kind]["frame"])
        # Rounding adds white noise of n * ROUND_POWER in all, spread
        # evenly from 0 to half the rate.
        nyq = rate / 2.0
        total = len(x) * ROUND_POWER
        fb = [total * width / nyq] * len(mb)
        f_low = total * min(TOP_HZ, nyq) / nyq
        ff = to_frames([ROUND_POWER] * len(x), step)
        m_all = sum(mspec)
        band_gain = _gain(m_low, f_low, p_low_model)
        frame_gain = _gain(sum(mf), sum(ff), sum(pf))
        if band_gain is None:
            band_gain = m_low / p_low_model if p_low_model > 0.0 else 0.0
            fb = [0.0] * len(fb)
        if frame_gain is None:
            frame_gain = sum(mf) / sum(pf) if sum(pf) > 0.0 else 0.0
            ff = [0.0] * len(ff)
        # The model part takes the frame gain, the floor counts only where
        # it applies.
        floor_on = sum(ff) > 0.0
        p_low = p_low_model * frame_gain + (f_low if floor_on else 0.0)
        p_all = sum(pspec) * frame_gain + (total if floor_on else 0.0)
    else:
        width, step = BAND_HZ, max(1, int(round(FRAME_S * rate)))
        nbands = int(min(TOP_HZ, rate / 2.0) // BAND_HZ)
        if nbands * BAND_HZ < min(TOP_HZ, rate / 2.0):
            nbands += 1
        # The last band also collects every bin above it, so drop it when
        # the rate reaches past 15 kHz.
        extra = rate / 2.0 > TOP_HZ
        nb = nbands + (1 if extra else 0)
        mb = band_energy(x, rate, nb)
        mf = frame_energy([v * v for v in x], rate)
        # Tones only: predict the rounded table itself, so no gain and no
        # white floor.
        r = rounded_tones(sound, rate)
        pb = band_energy(r, rate, nb)
        pf = frame_energy([v * v for v in r], rate)
        fb, ff = [0.0] * nb, [0.0] * len(pf)
        p_all = sum(pb)
        if extra:
            mb, pb, fb = mb[:-1], pb[:-1], fb[:-1]
        band_gain = frame_gain = 1.0
        m_low, m_all = sum(mb), sum(mf)
        p_low = sum(pb)
        keep_b, keep_f = [True] * len(mb), [True] * len(mf)
    be = stats(errors_db(mb, [p * band_gain + f for p, f in zip(pb, fb)], 1.0, keep_b)) \
        if mb else stats([])
    fe = stats(errors_db(mf, [p * frame_gain + f for p, f in zip(pf, ff)], 1.0, keep_f))
    # Share of the energy under 15 kHz, measured and predicted.
    m_share = m_low / m_all if m_all > 0.0 else 0.0
    p_share = p_low / p_all if p_all > 0.0 else 0.0
    level = None
    if has_noise:
        pmax = max(ps)
        if frame_gain > 0.0 and pmax > 0.0:
            level = 10.0 * math.log10(sound["peak_steps"] ** 2 / (frame_gain * pmax))
        else:
            level = float("inf")
        if not LEVEL_MIN_DB <= level <= LEVEL_MAX_DB:
            faults.append("level %.2f dB outside %.2f to %.2f"
                          % (level, LEVEL_MIN_DB, LEVEL_MAX_DB))
    bands_judged = False
    if (m_share < LOW_SHARE) != (p_share < LOW_SHARE):
        faults.append("power under 15 kHz is %.1f %% of the table, the model predicts %.1f %%"
                      % (100.0 * m_share, 100.0 * p_share))
        notes.append("bands not judged")
    elif m_share < LOW_SHARE:
        notes.append("bands not judged, no power under 15 kHz")
    elif has_noise and sum(keep_b) < MIN_COUNT:
        notes.append("bands not judged, too short")
    else:
        bands_judged = True
    frames_judged = not (has_noise and sum(keep_f) < MIN_COUNT)
    if not frames_judged:
        notes.append("frames not judged, too narrow")
    judged = (("band", be),) if bands_judged else ()
    judged += (("frame", fe),) if frames_judged else ()
    for what, st in judged:
        for stat, limit in RULES[kind][what]:
            if not st[stat] <= limit:
                faults.append("%s %s error %.2f dB over %.2f" % (what, stat, st[stat], limit))
    line = ("%s %s bands %.0f Hz, frames %.1f ms, band mean %.2f median %.2f worst %.2f dB, "
            "frame mean %.2f median %.2f worst %.2f dB%s%s: %s"
            % (name, kind, width, 1000.0 * step / rate,
               be["mean"], be["median"], be["worst"],
               fe["mean"], fe["median"], fe["worst"],
               ", level %.2f dB" % level if level is not None else "",
               "".join(", " + t for t in notes),
               "ok" if not faults else "FAIL " + "; ".join(faults)))
    return line, not faults


def main(argv):
    if len(argv) != 2 or any(a.startswith("-") for a in argv):
        sys.stderr.write("usage: sfx_check.py MODEL DIR\n")
        return EX_USAGE
    model_path, folder = argv
    try:
        model = load_model(model_path)
    except ModelError as e:
        sys.stderr.write("sfx_check.py: %s\n" % e)
        return EX_MODEL
    if not os.path.isdir(folder):
        sys.stderr.write("sfx_check.py: %s is not a folder\n" % folder)
        return EX_MODEL
    status = EX_OK
    for name in model["sounds"]:
        try:
            line, ok = check_sound(model, name, folder)
        except (TableError, ModelError) as e:
            sys.stderr.write("sfx_check.py: %s\n" % e)
            return EX_MODEL
        print(line)
        if not ok:
            status = EX_FAIL
    return status


if __name__ == "__main__":
    try:
        code = main(sys.argv[1:])
    except Exception as e:  # noqa: BLE001
        sys.stderr.write("sfx_check.py: internal error: %s: %s\n" % (type(e).__name__, e))
        code = EX_SOFTWARE
    sys.exit(code)
