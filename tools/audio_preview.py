#!/usr/bin/env python3
"""Rebuild what the audio pin would play from the audio trace of a serial log.

    audio_preview.py LOG OUT.wav --pcm DIR [--pcm DIR ...] [--scale MODE]
                  [--tone-peak N]

QEMU has no PWM model, so the kernel's audio runs on RobCo-Core's trace
backend and prints one marker per change. This tool reads those markers
and writes the pin's output as a WAV file, so a person can hear what a
kernel played.

Lines it reads, each an "@RC ... ev=audio op=..." marker of any stage:

  printed by every version of the library's trace:
    op=pcm rate= len= vol= crc= t_ms=    a sample play starts
    op=pcm ... loop=1                    a looped play starts, from
                                         newer versions only
    op=pcm_next len= crc= t_ms=          a queued buffer takes over
    op=pcm_end n= last= t_ms=            the play ends by itself
    op=tone hz= vol= t_ms=               a tone starts
    op=stop hz=0 vol= t_ms=              what plays stops, or a tone ends
  printed by newer versions only:
    op=pcm_mix len= crc= t_ms=           an overlay starts over the play
    op=voice rate= spans= len= vol= crc= t_ms=
                                         a voice starts: a note the
                                         library computes from spans
    ... gain=                            on op=pcm, op=voice or op=tone:
                                         the play's gain, 0 to 10000
    op=gain gain= t_ms=                  the gain of the play that runs
                                         moves toward a new target
    the rest ramp, a plain op=pcm rate=22050 len=882 vol=100 whose CRC is
    the library's ramp. The same bytes at another rate or volume are an
    ordinary play. Once the ramp has ended by itself, which prints its
    op=pcm_end, silence is duty 128, not duty 0. A ramp cut before that
    line, by a stop or a new sound, drops to duty 0 at once and leaves
    the rest level at duty 0, even when its last sample had sounded.
  ev=audio_drop n=                       records lost: a warning
  Each marker carries stage=. When the stage changes from one audio
  marker to the next, a new image starts and counts time from 0 again.
  What plays is cut there, as a reset cuts it, and the rest level goes
  back to duty 0, as for a freshly started driver. The new stage's times
  move so its first marker sits at the last marker of the stage before.
  The log does not hold the time of the handoff, so the tool adds no gap
  and warns once per change: "stage NEW follows stage OLD: the time
  between them is not in the log". Inside one stage, a time that goes
  back is a bad log. A reboot of the same image repeats its stage name
  with a lower time, so cut such a log in two before you render it.
  The driver prints ev=audio_drop after the records it drained, so it
  stands where the loss happened. After it, a marker whose start was lost
  is skipped: an end, a queued buffer or an overlay. A stop with nothing
  playing is counted too. A skipped marker still moves the time. One
  warning counts them. The next op=pcm, op=voice or op=tone
  ends the loss. Without a loss before it, such a marker is a bad log.
  A marker that fits the play active now is applied as usual, even after
  a loss: the tool cannot see a lost start that changed the play.
  A start whose end was lost needs nothing: the next start cuts it.
  The tool parses only a line with the field ev=audio or ev=audio_drop
  and skips every other marker untouched.
  An ev=audio marker without op= is a program's own status line, not
  trace: the tool skips it. An unknown op= is a bad log.

Rules it applies, from the library's driver:

- Sample bytes come from the .u8 files of the --pcm folders, found by the
  CRC-32 of their bytes and their length. An unknown pair plays as
  silence, a buffer of 128, with one warning per pair. The pin still moves
  to the midpoint for it.
- Mix: play + overlay - 128, clamped to 0 to 255, then the volume. A play
  can have many overlays, one at a time, each from its own start. One
  overlay runs at once: the library refuses a second while the first
  plays and prints no line, so such a line is a bad log. An overlay goes
  on across op=pcm_next and ends at its own end, or when its play ends
  or is cut.
- Loop: a play whose op=pcm line ends with loop=1 repeats its buffer
  until the trace ends it: op=stop, or a new op=pcm, op=voice or
  op=tone, which the driver prints after its op=stop. An overlay over a
  loop sounds on the play's grid across the wraps. The library refuses
  a queue on a loop and a loop never ends by itself, so op=pcm_next or
  op=pcm_end on a loop is a bad log.
- End of log: a tone or a loop the log leaves playing stops at the t_ms
  of the log's last marker, and the tail is silence at the rest level.
  A plain play or a voice plays to its own end, then the tail.
- Voice spans come from the .voice files of the --pcm folders, each
  the packed spans of one voice, found by the CRC-32 of their bytes and
  their length, 28 bytes a span. A file must pass the library's span
  checks, and its sum of n must equal len=. An unknown pair plays as
  silence, with one warning per pair. The tool renders a voice with the
  library's integer steps, from tools/voice_engine.py. An overlay over a
  voice adds to the voice on 1/256 of a step, then the volume applies
  and the sum rounds once to the nearest step, a half step away from
  128. --scale does not change a voice. A voice ends at op=pcm_end or
  op=stop. op=pcm_next on a voice is a bad log: the library refuses a
  queue while a voice plays.
- Volume: 128 + (s - 128) * vol / 100. --scale nearest, the default,
  rounds to the nearest step, a half step away from 128, as
  audio_pcm_scale() in src/lib/audio_pcm.c does. --scale cut rounds
  toward 128, for a log of a library older than that rounding.
  A play keeps the volume its line prints. An older driver applies a
  volume change to the samples that follow, but prints no line for it,
  so the file is wrong for such a play.
- Gain: a start line's gain= sets the play's gain in 1/10000. Without it
  the gain is vol * 100, and every rule below gives the same byte as
  the volume rule. A sample is 128 + (s - 128) * gain / 10000, rounded
  as --scale says. op=gain sets a new target. From the first sample of
  the play at or after its t_ms, each sample moves the gain toward the
  target by (500000 + rate - 1) / rate units, rate being the play's, or
  37500 for a tone, and never past it. The rest ramp keeps full scale
  and ignores op=gain. op=gain with nothing playing is a bad log. A
  voice is 128 + (q + (mix - 128) * 256) * gain / 2560000, rounded once.
- A sample play holds duty s / 256 of the 37.5 kHz carrier. Silence is
  duty 0, or duty 128 once the rest ramp has ended with its op=pcm_end.
- A tone before the rest ramp, or after a ramp cut early, is the pin's
  pulse: period round(9600000 / hz) counts, high for
  (period / 2 * vol + 50) / 100 of them. With a gain, vol is
  (gain + 50) / 100, taken when the note starts: op=gain does not change
  a pulse that plays.
- A tone that starts after the rest ramp reached its end is the
  library's centred square, from tools/tone_square.py, one sample per
  carrier period from the first frame at or after its t_ms:
    half = max(256, (37500 * 128 + hz / 2) / hz), so a pitch above
      18750 Hz plays at 18750 Hz
    amp = (peak * vol + 50) / 100 steps each side of 128
    a note starts at an edge going up; a sample that holds an edge takes
      the mean of the two levels by the edge's place in it
  With a gain, amp is (peak * gain + 5000) / 10000, taken every sample,
  so op=gain moves the height of a note that plays. The move starts at
  the first frame at or after the t_ms of op=gain.
  An op=tone right after another centred op=tone, with no op=stop
  between them, is the next note of the same play. It keeps the gain of
  the note before and its move, as the driver does, when its gain= or
  vol= equals the target of that move. Else it starts at its own gain,
  for a log of a driver that printed no op=gain. A note after op=stop,
  a rest of a sequence included, starts at its own gain. So does the
  first note. The gap between op=stop and the next op=tone holds 128. The
  library restarts a note after a late IRQ skips over 64 samples. The
  tool has no late IRQ, so that rule never applies here. The trace does
  not print the peak or the grid: the tool assumes the kernel's peak,
  127 unless --tone-peak says else, and a grid of 37500 Hz.
- Output: 16-bit mono at 37500 Hz, one frame per carrier period. A frame
  holds the sample due at its start, or the pulse's mean over the frame
  for a tone. Then a one-pole 20 Hz high-pass stands for the coupling
  capacitor, so a jump of the rest level is heard as a thump. A duty step
  of 100 percent is full scale, so a tone never clips.

Limits: a line of at most 4096 bytes, at most 1000000 lines, at most
300 s of output plus a 0.3 s tail, at most 4096 .u8 and .voice files of
at most 4 MiB each and 64 MiB together.
Each number is checked when its line is read, before anything is built
from it. From the library: rate= 4000 to 37500, hz= 20 to 20000, vol= 0
to 100, as the driver caps it before the trace prints it. gain= is 0 to
10000. len= is 1 to
11250000, 300 s at 37500 Hz, and so is a play with its queued buffers.
spans= is 1 to 149796, the spans of a 4 MiB file.
n= of op=pcm_end is at most 11250000 and last= below it. t_ms= is at most
300000. n= of ev=audio_drop is 0 to 4294967295.

Exit status: 0 on success, 2 on a bad log, a bad option or a file error,
with one line that says what and where.
"""

import argparse
import array
import importlib.util
import math
import os
import re
import struct
import sys
import zlib


def _load(name, file):
    """A module found beside this file. python -I leaves the script's
    folder off sys.path, so it is loaded by its path."""
    if name not in sys.modules:
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)), file)
        spec = importlib.util.spec_from_file_location(name, path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        sys.modules[name] = mod
    return sys.modules[name]


voice = _load("robco_voice_engine", "voice_engine.py")
square = _load("robco_tone_square", "tone_square.py")

OUT_RATE = 37500            # 9.6 MHz PWM clock / range 256
PWM_CLOCK = 9600000
HP_HZ = 20.0

MAX_LINE = 4096
MAX_LINES = 1000000
MAX_SECONDS = 300
MAX_FILES = 4096
MAX_FILE_BYTES = 4 << 20
MAX_PCM_BYTES = 64 << 20
TAIL_MS = 300

RAMP_LEN = 882
RAMP_RATE = 22050

# The library's bounds in include/robco/audio.h: AUDIO_PCM_RATE_MIN and _MAX,
# AUDIO_HZ_MIN and _MAX,
# and a volume of 0 to 100, as audio_set_volume() caps it before the trace
# records it. Its AUDIO_PCM_LEN_MAX is 2^31 - 1 bytes, more than the output
# can hold: a play is at most 300 s at the highest rate.
RATE_MIN = 4000
RATE_MAX = 37500
HZ_MIN = 20
HZ_MAX = 20000
VOL_MAX = 100
MAX_PCM_LEN = MAX_SECONDS * RATE_MAX
MAX_DROP = 0xffffffff
MAX_SPANS = MAX_FILE_BYTES // 28

FIELDS = {
    "pcm": ("rate", "len", "vol", "crc", "t_ms"),
    "pcm_next": ("len", "crc", "t_ms"),
    "pcm_end": ("n", "last", "t_ms"),
    "pcm_mix": ("len", "crc", "t_ms"),
    "voice": ("rate", "spans", "len", "vol", "crc", "t_ms"),
    "tone": ("hz", "vol", "t_ms"),
    "stop": ("hz", "vol", "t_ms"),
    "gain": ("gain", "t_ms"),
}
GAIN_MAX = 10000
# Starts that may carry gain=. On any other line the field is ignored.
GAIN_STARTS = ("pcm", "voice", "tone")
HEX_FIELDS = ("crc",)
# Times may go back this far: the tick's clock and the PCM end time are
# read apart and each drops its fraction of a millisecond.
BACK_SLACK_MS = 10

NUM = re.compile(r"[0-9]{1,10}\Z")
HEX = re.compile(r"[0-9a-fA-F]{8}\Z")


class LogError(Exception):
    pass


def ramp_bytes():
    """The library's rest ramp: 0 to 128 on 3t^2 - 2t^3, rounded to nearest,
    as ramp_fill() in src/drv/audio.c."""
    n = RAMP_LEN - 1
    n3 = n * n * n
    return bytes((128 * (3 * i * i * n - 2 * i * i * i) + n3 // 2) // n3
                 for i in range(RAMP_LEN))


RAMP = ramp_bytes()
RAMP_CRC = zlib.crc32(RAMP)


def is_ramp(op, f):
    """The library's rest ramp: its rate, volume, length and bytes."""
    return (op == "pcm" and not f.get("loop") and f["rate"] == RAMP_RATE and f["vol"] == 100
            and f["len"] == RAMP_LEN and f["crc"] == RAMP_CRC)


def scale_cut(s, vol, full=100):
    """128 + (s - 128) * vol / full, toward 128. full is 100 for a volume,
    GAIN_MAX for a gain."""
    d = (s - 128) * vol
    q = abs(d) // full
    return 128 + (q if d >= 0 else -q)


def scale_nearest(s, vol, full=100):
    """128 + (s - 128) * vol / full, a half step away from 128."""
    d = (s - 128) * vol
    h = full // 2
    return 128 + ((d + h) // full if d >= 0 else -((h - d) // full))


def voice_byte(q, mix, gain):
    """The voice plus an overlay byte, times a gain, one rounding. A gain
    of vol * 100 gives voice.out_byte(q, mix, vol)."""
    x = (q + (mix - 128) * 256) * min(gain, GAIN_MAX)
    r = (x + 1280000) // 2560000 if x >= 0 else -((1280000 - x) // 2560000)
    r += 128
    return 0 if r < 0 else 255 if r > 255 else r


def start_gain(f):
    """The gain a start line sets: its gain=, else vol * 100."""
    g = f.get("gain")
    return f["vol"] * 100 if g is None else g


class Slew:
    """The gain of one play, per sample. It starts at the target. Each
    change is (first sample, target). From that sample on, each sample
    moves the gain toward the target by `step`, and never past it."""

    def __init__(self, gain, rate):
        self.cur = self.target = gain
        self.step = (500000 + rate - 1) // rate
        self.changes = []
        self.n = 0              # changes taken
        self.i = -1             # the last sample computed

    def at(self, i):
        """The gain of sample i. Samples are asked in order, never back."""
        ch = self.changes
        while self.i < i:
            if self.cur == self.target:
                # Nothing moves until the next change: skip to it.
                nk = ch[self.n][0] if self.n < len(ch) else i + 1
                if nk > i:
                    self.i = i
                    break
                self.i = max(self.i, nk - 1)
            self.i += 1
            while self.n < len(ch) and ch[self.n][0] <= self.i:
                self.target = ch[self.n][1]
                self.n += 1
            if self.cur < self.target:
                self.cur = min(self.target, self.cur + self.step)
            elif self.cur > self.target:
                self.cur = max(self.target, self.cur - self.step)
        return self.cur


# ---- reading ----------------------------------------------------------

def parse_marker(text, where):
    """The ev=audio marker in `text`, as (op, fields), or None."""
    i = text.find("@RC ")
    if i < 0:
        return None
    toks = text[i + 4:].split()
    # Only an audio trace line is parsed. Any other marker is skipped
    # untouched, whatever its fields hold, such as a quoted string.
    if "ev=audio" not in toks and "ev=audio_drop" not in toks:
        return None
    kv = {}
    for tok in toks:
        if "=" not in tok:
            raise LogError("%s: a field without '=': %r" % (where, tok))
        k, v = tok.split("=", 1)
        if k in kv:
            raise LogError("%s: field %s twice" % (where, k))
        kv[k] = v
    ev = kv.get("ev")
    if ev == "audio_drop":
        n = kv.get("n")
        if n is None or not NUM.match(n) or int(n) > MAX_DROP:
            raise LogError("%s: ev=audio_drop needs n= from 0 to %d" % (where, MAX_DROP))
        return ("drop", {"n": int(n), "stage": kv.get("stage")})
    if ev != "audio":
        return None
    op = kv.get("op")
    if op is None:
        # A program's own status marker, such as ev=audio tick= rc=.
        return None
    if op not in FIELDS:
        raise LogError("%s: unknown audio op %r" % (where, op))
    out = {}
    for f in FIELDS[op]:
        v = kv.get(f)
        if v is None:
            raise LogError("%s: op=%s has no %s=" % (where, op, f))
        if f in HEX_FIELDS:
            if not HEX.match(v):
                raise LogError("%s: %s=%s is not 8 hex digits" % (where, f, v))
            out[f] = int(v, 16)
        else:
            if not NUM.match(v):
                raise LogError("%s: %s=%s is not a whole number" % (where, f, v))
            out[f] = int(v)
    if op in ("pcm", "pcm_next", "pcm_mix", "voice") and not 1 <= out["len"] <= MAX_PCM_LEN:
        raise LogError("%s: len=%d is outside 1 to %d" % (where, out["len"], MAX_PCM_LEN))
    if op == "pcm_end" and out["n"] > MAX_PCM_LEN:
        raise LogError("%s: n=%d is past %d" % (where, out["n"], MAX_PCM_LEN))
    if op == "pcm_end" and out["last"] >= MAX_PCM_LEN:
        raise LogError("%s: last=%d is past %d" % (where, out["last"], MAX_PCM_LEN - 1))
    if op == "voice" and not 1 <= out["spans"] <= MAX_SPANS:
        raise LogError("%s: spans=%d is outside 1 to %d" % (where, out["spans"], MAX_SPANS))
    if op in ("pcm", "voice") and not RATE_MIN <= out["rate"] <= RATE_MAX:
        raise LogError("%s: rate=%d is outside %d to %d"
                       % (where, out["rate"], RATE_MIN, RATE_MAX))
    if op in GAIN_STARTS and "gain" in kv:
        v = kv["gain"]
        if not NUM.match(v):
            raise LogError("%s: gain=%s is not a whole number" % (where, v))
        out["gain"] = int(v)
    if "gain" in out and out["gain"] > GAIN_MAX:
        raise LogError("%s: gain=%d is past %d" % (where, out["gain"], GAIN_MAX))
    if "vol" in out and out["vol"] > VOL_MAX:
        raise LogError("%s: vol=%d is past %d" % (where, out["vol"], VOL_MAX))
    if op == "tone" and not HZ_MIN <= out["hz"] <= HZ_MAX:
        raise LogError("%s: hz=%d is outside %d to %d" % (where, out["hz"], HZ_MIN, HZ_MAX))
    if op == "stop" and out["hz"] != 0:
        raise LogError("%s: op=stop with hz=%d, not 0" % (where, out["hz"]))
    if "loop" in kv:
        if op != "pcm" or kv["loop"] != "1":
            raise LogError("%s: loop=%s on op=%s: only op=pcm takes loop=1"
                           % (where, kv["loop"], op))
        out["loop"] = 1
    if out["t_ms"] > MAX_SECONDS * 1000:
        raise LogError("%s: t_ms=%d is past the %d s limit" % (where, out["t_ms"], MAX_SECONDS))
    out["stage"] = kv.get("stage")
    return (op, out)


def read_log(path):
    """[(where, op, fields)] in log order, and the count of dropped records."""
    events = []
    dropped = 0
    name = os.path.basename(path)
    with open(path, "rb") as f:
        no = 0
        while True:
            # Never read more of a line than the limit allows.
            raw = f.readline(MAX_LINE + 2)
            if not raw:
                break
            no += 1
            where = "%s:%d" % (name, no)
            if no > MAX_LINES:
                raise LogError("%s: more than %d lines" % (name, MAX_LINES))
            if len(raw) > MAX_LINE + 1:
                raise LogError("%s: line longer than %d bytes" % (where, MAX_LINE))
            text = raw.decode("utf-8", "replace").rstrip("\r\n")
            m = parse_marker(text, where)
            if m is None:
                continue
            if m[0] == "drop":
                dropped += m[1]["n"]
                # The loss keeps its place: the renderer skips the markers
                # whose start it took.
                if m[1]["n"]:
                    events.append((where, "drop", m[1]))
                continue
            events.append((where, m[0], m[1]))
    return events, dropped


def load_pcm(dirs):
    """{(crc, length): bytes} from the .u8 and .voice files of `dirs`.
    A .voice file is kept as its list of spans."""
    found = {}
    count = 0
    total = 0
    for d in dirs:
        try:
            names = sorted(os.listdir(d))
        except OSError as e:
            raise LogError("--pcm %s: %s" % (d, e.strerror))
        for n in names:
            is_voice = n.endswith(".voice")
            if not (n.endswith(".u8") or is_voice):
                continue
            count += 1
            if count > MAX_FILES:
                raise LogError("--pcm: more than %d .u8 and .voice files" % MAX_FILES)
            p = os.path.join(d, n)
            size = os.path.getsize(p)
            if size > MAX_FILE_BYTES:
                raise LogError("%s: larger than %d bytes" % (p, MAX_FILE_BYTES))
            total += size
            if total > MAX_PCM_BYTES:
                raise LogError("--pcm: the .u8 and .voice files pass %d bytes together"
                               % MAX_PCM_BYTES)
            with open(p, "rb") as f:
                data = f.read(MAX_FILE_BYTES + 1)
            if len(data) > MAX_FILE_BYTES:
                raise LogError("%s: larger than %d bytes" % (p, MAX_FILE_BYTES))
            if not data and is_voice:
                raise LogError("%s: no span" % p)
            if not data:
                continue
            key = (zlib.crc32(data), len(data))
            if is_voice:
                try:
                    spans = voice.unpack(data)
                except ValueError as e:
                    raise LogError("%s: %s" % (p, e))
                bad = voice.check(spans)
                if bad:
                    raise LogError("%s: %s" % (p, bad))
                found.setdefault(("voice",) + key, spans)
            else:
                found.setdefault(key, data)
    return found


# ---- rendering --------------------------------------------------------

def frame_of(ms):
    return (ms * OUT_RATE + 999) // 1000


class Play:
    """A sample play: the chained buffers as one stream, and its overlay."""

    def __init__(self, t0, rate, vol, data, ramp, loop=False, gain=None):
        self.t0 = t0
        self.loop = loop
        self.rate = rate
        self.vol = 100 if ramp else vol
        if ramp or gain is None:
            gain = self.vol * 100
        self.slew = Slew(gain, rate)
        self.data = bytearray(data)
        self.ramp = ramp
        self.mixes = []         # [(first sample index, bytes)], in order, apart
        self.ended = False

    def index(self, frame):
        """The sample due at the start of `frame`."""
        return ((frame * 1000 - self.t0 * OUT_RATE) * self.rate) // (OUT_RATE * 1000)

    def sample_at(self, t):
        """The first sample at or after time t."""
        return -((self.t0 - t) * self.rate // 1000)

    def set_gain(self, t, gain):
        if not self.ramp:       # the rest ramp keeps full scale
            self.slew.changes.append((self.sample_at(t), gain))

    def end_ms(self):
        return self.t0 + (len(self.data) * 1000 + self.rate - 1) // self.rate


class VoicePlay(Play):
    """A voice: samples from spans, and its overlays."""

    def __init__(self, t0, rate, vol, spans, length, gain=None):
        Play.__init__(self, t0, rate, vol, b"", False, gain=gain)
        self.spans = spans
        self.length = length

    def end_ms(self):
        return self.t0 + (self.length * 1000 + self.rate - 1) // self.rate


class Tone:
    """A tone. Centred once the rest ramp has reached its end, else the pulse.
    A centred note that follows a centred note with no op=stop between them
    takes that note's Slew, `carry`: the gain and its move go on."""

    def __init__(self, t0, hz, vol, centred=False, peak=square.TONE_PEAK, gain=None,
                 carry=None):
        self.t0 = t0
        self.centred = centred
        self.hz = hz
        self.peak = peak
        if gain is None:
            gain = vol * 100
        # The pulse takes its width once per note. With gain vol * 100 the
        # width is today's.
        pvol = (gain + 50) // 100
        self.amp = square.amp_gain(peak, gain)
        # The Slew of a centred tone counts output frames from frame 0, so
        # the notes of one play share it.
        self.slew = carry if carry is not None else Slew(gain, square.TONE_RATE)
        self.rng = max(2, (PWM_CLOCK + hz // 2) // hz)
        self.duty = ((self.rng // 2) * pvol + 50) // 100 / self.rng

    def set_gain(self, t, gain):
        # The move starts at the first frame at or after t. A pulse keeps
        # the width of its note.
        if self.centred:
            self.slew.changes.append((frame_of(t), gain))

    def target(self):
        """The target of the gain after the changes so far."""
        ch = self.slew.changes
        return ch[-1][1] if ch else self.slew.target


class Renderer:
    def __init__(self, pcm, scale, warn, peak=square.TONE_PEAK):
        self.pcm = pcm
        self.peak = peak
        self.scale = scale
        self.warn = warn
        self.unknown = set()
        self.silence = b""
        self.unknown_voice = set()
        self.duty = array.array("f")
        self.rest = 0.0
        self.cur = None

    def lookup(self, crc, length):
        data = self.pcm.get((crc, length))
        if data is not None:
            return data
        if length == RAMP_LEN and crc == RAMP_CRC:
            return RAMP
        if (crc, length) not in self.unknown:
            self.unknown.add((crc, length))
            self.warn("no .u8 file has crc=%08x len=%d: it plays as silence" % (crc, length))
        # One shared buffer of silence, viewed, not copied per line.
        if len(self.silence) < length:
            self.silence = bytes([128]) * length
        return memoryview(self.silence)[:length]

    def lookup_voice(self, crc, spans, length, where):
        """The spans of a voice, or None for an unknown one."""
        found = self.pcm.get(("voice", crc, spans * voice.SPAN_BYTES))
        if found is None:
            if (crc, spans) not in self.unknown_voice:
                self.unknown_voice.add((crc, spans))
                self.warn("no .voice file has crc=%08x and %d spans: it plays as silence"
                          % (crc, spans))
            return None
        if voice.length(found) != length:
            raise LogError("%s: len=%d, but the spans of crc=%08x hold %d samples"
                           % (where, length, crc, voice.length(found)))
        return found

    def fill_to(self, frame, level):
        n = frame - len(self.duty)
        if n > 0:
            self.duty.extend([level] * n)

    def close(self, t):
        """Renders what plays up to time t, then silence takes over."""
        end = frame_of(t)
        if end > (MAX_SECONDS * 1000 + TAIL_MS) * OUT_RATE // 1000:
            raise LogError("the output would pass %d s" % MAX_SECONDS)
        cur = self.cur
        self.cur = None
        if cur is None:
            self.fill_to(end, self.rest)
            return
        start = len(self.duty)
        if isinstance(cur, Tone):
            self.render_tone(cur, start, end)
        elif isinstance(cur, VoicePlay):
            self.render_voice(cur, start, end)
        else:
            self.render_play(cur, start, end)

    def render_tone(self, tone, start, end):
        if tone.centred:
            self.render_square(tone, start, end)
            return
        d = tone.duty
        per = PWM_CLOCK / tone.rng          # pulse periods per second
        out = self.duty
        t0 = tone.t0 / 1000.0

        def high(x):
            k = math.floor(x)
            return k * d + min(x - k, d)

        if d == 0:              # volume 0: the driver writes silence
            out.extend([self.rest] * max(0, end - start))
            return
        for f in range(start, end):
            a = (f / OUT_RATE - t0) * per
            b = ((f + 1) / OUT_RATE - t0) * per
            if b <= 0:
                out.append(self.rest)
            else:
                a = max(a, 0.0)
                out.append((high(b) - high(a)) / (b - a))

    def render_square(self, tone, start, end):
        """The library's centred square: grid sample 0 is the first frame
        at or after the tone's start. The tool has no late IRQ, so no
        sample is ever skipped."""
        out = self.duty
        first = frame_of(tone.t0)
        q = square.Square(tone.hz, tone.amp, OUT_RATE)
        sl = tone.slew
        g = None
        for f in range(start, end):
            if f < first:
                out.append(self.rest)
            else:
                k = sl.at(f) if sl.changes else sl.cur
                if k != g:
                    g = k
                    q.amp = square.amp_gain(tone.peak, g)
                out.append(q.next() / 256.0)

    def render_play(self, p, start, end):
        out = self.duty
        n = len(p.data)
        mixes = p.mixes
        m = 0                   # the overlay that plays now or comes next
        mix0, mix = mixes[0] if mixes else (n, b"")
        scale = self.scale
        cache = {}
        sl = p.slew
        g = sl.cur
        after = 0.5 if p.ramp else self.rest
        for f in range(start, end):
            i = p.index(f)
            if i < 0:
                out.append(self.rest)
                continue
            if i >= n:
                if not p.loop:
                    out.append(after)
                    continue
                s = p.data[i % n]
            else:
                s = p.data[i]
            j = i - mix0
            while j >= len(mix) and m + 1 < len(mixes):
                m += 1
                mix0, mix = mixes[m]
                j = i - mix0
            if 0 <= j < len(mix):
                s = min(255, max(0, s + mix[j] - 128))
            if sl.changes:
                k = sl.at(i)
                if k != g:
                    g = k
                    cache = {}
            v = cache.get(s)
            if v is None:
                # With gain vol * 100 this is the byte of scale(s, vol).
                v = cache[s] = scale(s, g, GAIN_MAX) / 256.0
            out.append(v)
        if p.ramp and p.ended:
            # The library sets the midpoint rest when the ramp ends by itself,
            # which prints op=pcm_end. A cut before that line, even after the
            # last sample, leaves duty 0.
            self.rest = 0.5

    def render_voice(self, p, start, end):
        """One rounding: the voice on 1/256 of a step plus the overlay,
        times the volume, as the library's audio_voice_out()."""
        out = self.duty
        n = p.length
        mixes = p.mixes
        m = 0
        mix0, mix = mixes[0] if mixes else (n, b"")
        v = voice.Voice(p.spans) if p.spans is not None else None
        for f in range(start, end):
            i = p.index(f)
            if i < 0:
                out.append(self.rest)
                continue
            if i >= n:
                out.append(self.rest)
                continue
            q = v.at(i) if v is not None else 0
            j = i - mix0
            while j >= len(mix) and m + 1 < len(mixes):
                m += 1
                mix0, mix = mixes[m]
                j = i - mix0
            o = mix[j] if 0 <= j < len(mix) else 128
            out.append(voice_byte(q, o, p.slew.at(i)) / 256.0)

    def lost_end(self):
        if self.lost is not None and self.lost[1]:
            self.warn("%s: after the lost records, %d markers had no start: skipped"
                      % self.lost)
        self.lost = None

    def misplaced(self, op, cur, where):
        """The error text when `op` does not fit the play active now, else None.
        Both paths use it: outside a loss the error is raised, inside one the
        marker is skipped."""
        if op == "pcm_next":
            if not isinstance(cur, Play) or cur.ended:
                return "%s: op=pcm_next with no sample play" % where
            if cur.ramp:
                return "%s: op=pcm_next on the rest ramp" % where
            if cur.loop:
                return ("%s: op=pcm_next on a loop: the library refuses a "
                        "queue while a loop plays" % where)
            if isinstance(cur, VoicePlay):
                return ("%s: op=pcm_next on a voice: the library refuses a "
                        "queue while a voice plays" % where)
        elif op == "pcm_mix":
            if not isinstance(cur, Play) or cur.ended:
                return "%s: op=pcm_mix with no sample play or voice" % where
            if cur.ramp:
                return "%s: op=pcm_mix on the rest ramp" % where
        elif op == "pcm_end":
            if not isinstance(cur, Play) or cur.ended:
                return "%s: op=pcm_end with no sample play or voice" % where
            if cur.loop:
                return "%s: op=pcm_end on a loop: a loop never ends by itself" % where
        elif op == "gain":
            if cur is None or (isinstance(cur, Play) and cur.ended):
                return ("%s: op=gain with nothing playing: the driver prints "
                        "no line then" % where)
        return None

    def run(self, events):
        last = 0
        self.lost = None        # (where, markers skipped) after a loss
        stage = None
        seen = False
        shift = 0               # added to t_ms in this stage
        rebase = False          # the next t_ms of this stage sets shift
        for where, op, f in events:
            if seen and f.get("stage") != stage:
                # A new image starts its tick count again. What plays is
                # cut, as a reset cuts it, and the driver starts fresh.
                self.warn("stage %s follows stage %s: the time between them is not in the log"
                          % (f.get("stage"), stage))
                self.lost_end()
                self.lost = None
                self.close(last)
                self.rest = 0.0
                rebase = True
            stage = f.get("stage")
            seen = True
            if op == "drop":
                self.lost_end()
                self.lost = (where, 0)
                continue
            cur = self.cur
            # Every marker moves the time, a skipped one too.
            if rebase:
                shift = last - f["t_ms"]
                rebase = False
            t = f["t_ms"] + shift
            if t + BACK_SLACK_MS < last:
                raise LogError("%s: t_ms=%d comes before t_ms=%d of an earlier line"
                               % (where, f["t_ms"], last - shift))
            t = max(t, last)
            last = t
            if self.lost is not None:
                if op in ("pcm", "voice", "tone"):
                    self.lost_end()
                elif (cur is None if op == "stop"
                      else self.misplaced(op, cur, where) is not None):
                    # A marker whose start was lost changes no sound, but
                    # its time still counts.
                    self.lost = (self.lost[0], self.lost[1] + 1)
                    if op != "stop":
                        continue
            bad = self.misplaced(op, cur, where)
            if bad is not None:
                raise LogError(bad)
            if op == "pcm":
                self.close(t)
                ramp = is_ramp(op, f)
                if ramp:
                    # The library starts a ramp from duty 0 only. Cut before
                    # its op=pcm_end, it leaves duty 0, also after an
                    # earlier ramp that reached its end.
                    self.rest = 0.0
                data = self.lookup(f["crc"], f["len"])
                self.cur = Play(t, f["rate"], f["vol"], data, ramp, bool(f.get("loop")),
                                start_gain(f))
            elif op == "voice":
                self.close(t)
                spans = self.lookup_voice(f["crc"], f["spans"], f["len"], where)
                self.cur = VoicePlay(t, f["rate"], f["vol"], spans, f["len"], start_gain(f))
            elif op == "gain":
                cur.set_gain(t, f["gain"])
            elif op == "pcm_next":
                if len(cur.data) + f["len"] > MAX_PCM_LEN:
                    raise LogError("%s: the play's buffers pass %d bytes" % (where, MAX_PCM_LEN))
                cur.data += self.lookup(f["crc"], f["len"])
            elif op == "pcm_mix":
                k = -((cur.t0 - t) * cur.rate // 1000)      # first grid point at or after t
                if cur.mixes:
                    busy = cur.mixes[-1][0] + len(cur.mixes[-1][1])
                    if k < busy:
                        # t_ms drops the fraction of a millisecond, so the start
                        # may lie up to one ms later than k says.
                        if k + (cur.rate + 999) // 1000 < busy:
                            raise LogError("%s: op=pcm_mix while an overlay plays: the "
                                           "library refuses it and prints nothing" % where)
                        k = busy
                cur.mixes.append((k, self.lookup(f["crc"], f["len"])))
            elif op == "pcm_end":
                cur.ended = True
            elif op == "tone":
                prev = self.cur
                gain = start_gain(f)
                carry = None
                if isinstance(prev, Tone) and prev.centred and prev.target() == gain:
                    # The next note of the same play: the driver keeps the
                    # gain and its move. A line whose gain differs from the
                    # target comes from a driver that prints no op=gain: it
                    # starts at its own gain, as before.
                    carry = prev.slew
                self.close(t)
                # A tone is centred when the rest ramp reached its end before it.
                self.cur = Tone(t, f["hz"], f["vol"], self.rest == 0.5, self.peak, gain, carry)
            else:  # stop
                self.close(t)
        self.lost_end()
        cur = self.cur
        stop = last
        if isinstance(cur, Play) and not cur.loop:
            # The driver ends a plain play or a voice by itself, at a time
            # its start and length fix.
            stop = max(stop, cur.end_ms())
        else:
            # A tone or a loop needs a stop: it stops at the last marker.
            self.close(last)
        self.close(stop + TAIL_MS)


def high_pass(duty):
    """One-pole high-pass at HP_HZ over a duty from 0 to 1, as 16-bit samples."""
    rc = 1.0 / (2 * math.pi * HP_HZ)
    a = rc / (rc + 1.0 / OUT_RATE)
    out = array.array("h", bytes(2 * len(duty)))
    y = 0.0
    x0 = 0.0                # the pin rests low before the first sound
    clipped = 0
    for i, x in enumerate(duty):
        y = a * (y + x - x0)
        x0 = x
        v = int(round(y * 32767))
        if v > 32767:
            v, clipped = 32767, clipped + 1
        elif v < -32767:
            v, clipped = -32767, clipped + 1
        out[i] = v
    return out, clipped


def write_wav(path, samples):
    if sys.byteorder != "little":
        samples = array.array("h", samples)
        samples.byteswap()
    data = samples.tobytes()
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, OUT_RATE, OUT_RATE * 2, 2, 16))
        f.write(b"data" + struct.pack("<I", len(data)))
        f.write(data)


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="audio_preview.py",
        description=__doc__.split("\n\n", 1)[1],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", metavar="LOG", help="a serial log with ev=audio markers")
    ap.add_argument("out", metavar="OUT.wav")
    ap.add_argument("--pcm", metavar="DIR", action="append", default=[], required=True,
                    help="a folder of .u8 and .voice files, such as from sfx_table.py --dump")
    ap.add_argument("--tone-peak", metavar="N", type=int, default=square.TONE_PEAK,
                    help="ROBCO_AUDIO_TONE_PEAK of the kernel that made the log, 0 to %d "
                         "(default %d); the trace does not print it"
                    % (square.PEAK_MAX, square.TONE_PEAK))
    ap.add_argument("--scale", choices=("nearest", "cut"), default="nearest",
                    help="how the volume rounds: nearest, as the driver does, or cut, "
                         "for a log of an older library")
    a = ap.parse_args(argv)
    if not 0 <= a.tone_peak <= square.PEAK_MAX:
        ap.error("--tone-peak must be 0 to %d" % square.PEAK_MAX)

    def warn(msg):
        print("audio_preview.py: warning: %s" % msg, file=sys.stderr)

    try:
        events, dropped = read_log(a.log)
        markers = sum(1 for _, op, _ in events if op != "drop")
        pcm = load_pcm(a.pcm)
        scale = a.scale
        if dropped:
            warn("the trace lost %d records: the file misses sounds" % dropped)
        if not markers:
            warn("the log has no ev=audio marker: the file is silent")
        r = Renderer(pcm, scale_nearest if scale == "nearest" else scale_cut, warn,
                     a.tone_peak)
        r.run(events)
        samples, clipped = high_pass(r.duty)
        if clipped:
            warn("%d samples clipped" % clipped)
        write_wav(a.out, samples)
    except LogError as e:
        print("audio_preview.py: %s" % e, file=sys.stderr)
        return 2
    except OSError as e:
        print("audio_preview.py: %s: %s" % (e.filename or "", e.strerror), file=sys.stderr)
        return 2
    print("audio_preview.py: %s: %.2f s, %d markers, scale %s"
          % (a.out, len(samples) / OUT_RATE, markers, scale))
    return 0


if __name__ == "__main__":
    sys.exit(main())
