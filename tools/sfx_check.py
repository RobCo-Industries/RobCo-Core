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


def _need_pairs(obj, key, where, lo, hi, optional=False):
    if key not in obj:
        if optional:
            return
        raise ModelError("%s: missing %s" % (where, key))
    v = obj[key]
    if (not isinstance(v, list) or not lo <= len(v) <= hi
            or not all(isinstance(p, list) and len(p) == 2
                       and _is_int(p[0]) and _is_int(p[1]) for p in v)):
        raise ModelError("%s: %s must be %d to %d integer pairs" % (where, key, lo, hi))


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
        _need_int(s, "len", where, 1, 262144)
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
            _need_pairs(layer, "hits", lw, 1, 64)
            _need_int(layer, "level_mdb", lw, -120000, 20000)
            for start, _ in layer["hits"]:
                if not 0 <= start < s["len"]:
                    raise ModelError("%s: a hit starts outside the sound" % lw)
            if layer["kind"] == "tone":
                _need_int(layer, "len", lw, 1, 262144)
                pm = layer.get("pitch_mhz")
                top = model["rate_hz"] * 500
                if (not isinstance(pm, list) or len(pm) != 2
                        or not all(_is_int(p) and 1 <= p <= top for p in pm)):
                    raise ModelError("%s: pitch_mhz must be two integers from 1 to %d" % (lw, top))
                _need_pairs(layer, "partials", lw, 1, 16)
                _need_int(layer, "attack_us", lw, 0, 32768000, optional=True)
                _need_int(layer, "release_us", lw, 0, 32768000, optional=True)
                _need_pairs(layer, "env", lw, 1, 64, optional=True)
            else:
                _need_pairs(layer, "env", lw, 2, 64)
                _need_pairs(layer, "bands", lw, 2, 24)
                _need_int(layer, "fold", lw, 0, 262144, optional=True)
                if "repeat" in layer and not isinstance(layer["repeat"], bool):
                    raise ModelError("%s: repeat must be true or false" % lw)
            env = layer.get("env", [])
            if any(env[k][0] > env[k + 1][0] for k in range(len(env) - 1)) or \
                    any(t < 0 for t, _ in env):
                raise ModelError("%s: env times must start at 0 or later and not go down" % lw)
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


def filter_bands(bands_pts, rate, nbands):
    """Fraction of the noise filter's power in each 250 Hz band."""
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
    frac = band_energy(h + [0.0] * (FILTER_FFT - FIR_TAPS), rate, nbands)
    total = sum(frac)
    if total <= 0.0:
        raise ModelError("a noise layer's bands give no level")
    return [v / total for v in frac]


def noise_power(layer, sound, rate):
    """Expected power of a noise layer at each sample of the sound."""
    n = ceil_div(layer["env"][-1][0] * rate, 1000000)
    gain = env_gain(layer["env"], n, rate, False)
    fold = layer.get("fold", 0)
    for i in range(fold):
        u = (i + 0.5) / fold
        gain[i] *= math.sin(math.pi / 2 * u)
        gain[n - fold + i] *= math.cos(math.pi / 2 * u)
    sq = [g * g for g in gain]
    power = [0.0] * sound["len"]
    for start, level in layer["hits"]:
        _add(power, start, sq, db_gain((layer["level_mdb"] + level) / 500.0),
             sound.get("loop", False))
    return power


def predict(sound, rate, nbands):
    """Band and frame energy of a sound, from its model, before scaling."""
    loop = sound.get("loop", False)
    tone = [0.0] * sound["len"]
    has_tone = False
    noise_bands = [0.0] * nbands
    noise_pow = [0.0] * sound["len"]
    for layer in sound["layers"]:
        if layer["kind"] == "tone":
            has_tone = True
            one = tone_hit(layer, sound, rate)
            for start, level in layer["hits"]:
                _add(tone, start, one, db_gain(level / 1000.0), loop)
        else:
            power = noise_power(layer, sound, rate)
            frac = filter_bands(layer["bands"], rate, nbands)
            total = sum(power)
            for b in range(nbands):
                noise_bands[b] += total * frac[b]
            for i, p in enumerate(power):
                noise_pow[i] += p
    bands = band_energy(tone, rate, nbands) if has_tone else [0.0] * nbands
    bands = [a + b for a, b in zip(bands, noise_bands)]
    frames = frame_energy([t * t + p for t, p in zip(tone, noise_pow)], rate)
    return bands, frames


def rounding_floor(n, rate, nbands):
    """Band and frame energy of the rounding noise of an n sample table.

    The bands of band_energy() sum to the sum of the squared samples, so the
    noise gives n * ROUND_POWER in all, spread by width from 0 to half the
    rate. The last band reaches to half the rate, as in band_energy().
    """
    nyq = rate / 2.0
    total = n * ROUND_POWER
    bands = []
    for b in range(nbands):
        lo = min(b * BAND_HZ, nyq)
        hi = nyq if b == nbands - 1 else min((b + 1) * BAND_HZ, nyq)
        bands.append(total * (hi - lo) / nyq)
    return bands, frame_energy([ROUND_POWER] * n, rate)


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


def errors_db(meas, pred, gain):
    """dB errors over the entries within FLOOR_DB of the strongest measured."""
    top = max(meas)
    if top <= 0.0:
        return []
    out = []
    for m, p in zip(meas, pred):
        if m <= 0.0 or 10.0 * math.log10(top / m) > FLOOR_DB:
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


def check_sound(model, name, folder):
    """Return the result line of one sound and whether it passes."""
    sound = model["sounds"][name]
    rate = model["rate_hz"]
    data = read_table(name, sound, folder)
    kind = kind_of(sound)
    faults = []
    peak = max(abs(b - 128) for b in data)
    if peak != sound["peak_steps"]:
        faults.append("peak is %d steps, the model says %d" % (peak, sound["peak_steps"]))
    nbands = int(min(TOP_HZ, rate / 2.0) // BAND_HZ)
    if nbands * BAND_HZ < min(TOP_HZ, rate / 2.0):
        nbands += 1
    # The last band also collects every bin above it, so drop it when the
    # rate reaches past 15 kHz.
    extra = rate / 2.0 > TOP_HZ
    x = [b - 128.0 for b in data]
    mb = band_energy(x, rate, nbands + (1 if extra else 0))
    mf = frame_energy([v * v for v in x], rate)
    pb, pf = predict(sound, rate, nbands + (1 if extra else 0))
    fb, ff = rounding_floor(len(data), rate, nbands + (1 if extra else 0))
    if extra:
        mb, pb, fb = mb[:-1], pb[:-1], fb[:-1]
    if sum(pb) > 0.0 and sum(mb) > sum(fb):
        gain = (sum(mb) - sum(fb)) / sum(pb)
    else:
        gain = sum(mb) / sum(pb) if sum(pb) > 0.0 else 0.0
        fb, ff = [0.0] * len(fb), [0.0] * len(ff)
    be = stats(errors_db(mb, [p * gain + f for p, f in zip(pb, fb)], 1.0))
    fe = stats(errors_db(mf, [p * gain + f for p, f in zip(pf, ff)], 1.0))
    for what, st in (("band", be), ("frame", fe)):
        for stat, limit in RULES[kind][what]:
            if not st[stat] <= limit:
                faults.append("%s %s error %.2f dB over %.2f" % (what, stat, st[stat], limit))
    line = ("%s %s band mean %.2f median %.2f worst %.2f dB, "
            "frame mean %.2f median %.2f worst %.2f dB: %s"
            % (name, kind, be["mean"], be["median"], be["worst"],
               fe["mean"], fe["median"], fe["worst"],
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
