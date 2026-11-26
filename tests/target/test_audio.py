"""Boot examples/audio through boot() on every board: backend choice and trace markers."""

import math
import re
import struct
import zlib

from harness.boot import boot

DONE = re.compile(r"@RC stage=audio ev=done\n")
REPLAYED = re.compile(r"ev=done\n(?:.*\n)*?.*ev=case_end name=seq .*\n")
TICK_MS = 10  # the example runs a 100 Hz tick
# A trace stamp is the audio clock in whole ms, truncated, so it reads at
# most 1 ms less than the true time and never more.
STAMP_MS = 1


def call_min(ms):
    """Least stamp gap between a sound start and the stop a call makes ms later.

    The main context asks at c0 and stops at c1 >= c0 + ms. Each takes
    effect at the next tick run, so the start stamp's true time is at most
    TICK_MS after c0, plus the tick IRQ latency, and the stop's is after c1.
    The true gap is then more than ms - TICK_MS - latency. Truncation makes
    the stamp gap more than the true gap minus STAMP_MS. With a latency up
    to 1 ms, the integer stamp gap is at least ms - TICK_MS - STAMP_MS.
    """
    return ms - TICK_MS - STAMP_MS


# An allowance for late interrupt delivery by a loaded QEMU host. It is not a
# guarantee of the driver. call_min() assumes the tick IRQ runs within 1 ms of
# its boundary, which holds on hardware and on an idle host. A loaded host can
# deliver a tick late, so a start is stamped late and the gap reads short.
# Seen: a 1 ms miss under four parallel QEMU jobs. If a bound fails by more
# than this under heavy load, rerun the test alone before changing the number.
HOST_LATE_MS = 5


def call_floor(ms):
    """call_min(ms) less HOST_LATE_MS: the bound the tests assert."""
    return call_min(ms) - HOST_LATE_MS

# QEMU models no PWM: reads abort on the BCM2835 models, the others ignore writes.
PROBE = {"raspi0": "abort", "raspi1ap": "abort", "raspi2b": "ignored", "raspi3b": "ignored"}

# Expected tones per case as (hz, nominal ms); 0 is silence.
BEEP = [(1000, 80), (0, 0)]
SEQ = [(523, 120), (659, 120), (784, 200), (0, 0)]
# terminal:d=8,o=5,b=200: an eighth lasts 150 ms.
TUNE = [(523, 150), (784, 150), (659, 150), (0, 150), (1047, 300), (880, 150), (784, 450),
        (0, 0)]
LOOP = [(660, 60), (0, 40), (990, 60)]

TONE_CASES = ["beep", "seq", "tune", "loop", "interrupt"]
# The trace rule cases, each run RULE_RUNS times in a row.
RULE_RUNS = 5
RULE_CASES = ["stop_pcm"] * RULE_RUNS + ["stop_rest"] * RULE_RUNS
PCM_CASES = ["pcm", "pcm_fast", "pcm_top", "pcm_stop", "pcm_swap", "pcm_cut", "pcm_loop", "pcm_tick",
             "pcm_args", "pcm_chain", "pcm_qerr", "pcm_qcut", "pcm_under", "pcm_mix", "pcm_voice"]
# Last in the run: the rest level then stays at the midpoint.
REST_CASES = ["rest"]
ALL_CASES = TONE_CASES + PCM_CASES + RULE_CASES + REST_CASES

# AUDIO_ERR_* codes.
ERR_NO_INIT, ERR_RANGE, ERR_ARG, ERR_BUSY, ERR_STATE = -1, -6, -7, -8, -11

# The pcm_chain chunks of examples/audio/main.c as (offset, length), at 8000 Hz.
CHAIN = [(0, 800), (800, 600), (1400, 1000)]
PCM_BUF = 22050

# A late end is fine under load; this only catches a play that never ends on time.
PCM_END_SLACK_MS = 1000
# A late IRQ skips samples, and the host can stall the guest for some ms near the end.
# Under load the last sample landed 3 to 4 ms before the end.
PCM_LAST_SLACK_MS = 20
# A busy call with a 16 MB buffer: a CRC over it took 89 to 119 ms in QEMU.
BUSY_BIG_MAX_US = 30000


# The audio_rest_mid() ramp: 882 samples at 22050 Hz, 0 to 128 on 3t^2 - 2t^3.
RAMP_LEN, RAMP_RATE = 882, 22050


def ramp_data():
    """The ramp src/drv/audio.c builds, in the same integer maths."""
    n = RAMP_LEN - 1
    n3 = n ** 3
    return bytes((128 * (3 * i * i * n - 2 * i ** 3) + n3 // 2) // n3 for i in range(RAMP_LEN))


def pcm_data(n):
    """The buffer examples/audio/main.c pcm_fill() builds."""
    out = bytearray()
    for i in range(n):
        v = ((i * (64 + (i >> 6))) >> 4) & 0xFF
        out.append(v * 2 if v < 128 else (255 - v) * 2 + 1)
    return bytes(out)


def events(vm, name):
    return [m for m in vm.markers() if m.get("stage") == "audio" and m.get("ev") == name]


def cases(vm):
    """Return {case name: [audio markers]} in serial order; the last run of a name wins."""
    out = {}
    cur = None
    for m in vm.markers():
        if m.get("stage") != "audio":
            continue
        ev = m.get("ev")
        if ev == "case":
            cur = m["name"]
            out[cur] = []
        elif ev == "case_end":
            assert m["name"] == cur, m
            cur = None
        elif ev == "audio":
            assert cur is not None, m
            out[cur].append(m)
    return out


def tones(marks):
    return [(int(m["hz"]) if m["op"] == "tone" else 0, int(m["t_ms"])) for m in marks]


def check_pcm_start(m, rate, n, vol):
    assert m["op"] == "pcm", m
    assert (int(m["rate"]), int(m["len"]), int(m["vol"])) == (rate, n, vol), m
    assert m["crc"] == "%08x" % zlib.crc32(pcm_data(n)), m


def check_pcm_play(marks, rate, n, vol):
    """A whole play: start, then the end after n / rate seconds."""
    assert [m["op"] for m in marks] == ["pcm", "pcm_end"], marks
    start, end = marks
    check_pcm_start(start, rate, n, vol)
    nominal = n * 1000 // rate
    took = int(end["t_ms"]) - int(start["t_ms"])
    assert nominal <= took < nominal + PCM_END_SLACK_MS, (took, nominal)
    last = int(end["last"])
    got = int(end["n"])
    # The library does not promise an IRQ per sample: QEMU may deliver far fewer.
    # At least one IRQ per 2 ms proves it fired through the whole play.
    assert nominal // 2 <= got <= n, (end, nominal)
    # The last sample output lies in the final PCM_LAST_SLACK_MS of the buffer.
    assert max(0, n - 1 - rate * PCM_LAST_SLACK_MS // 1000) <= last <= n - 1, end


def check_pcm(vm, c):
    check_pcm_play(c["pcm"], 8000, 800, 100)
    check_pcm_play(c["pcm_fast"], 22050, 2205, 50)
    # The top rate, one sample per PWM carrier period.
    check_pcm_play(c["pcm_top"], 37500, 3750, 100)

    # Stopped after 200 ms of a 1 s play: a stop line, no pcm_end.
    marks = c["pcm_stop"]
    assert [m["op"] for m in marks] == ["pcm", "stop"], marks
    check_pcm_start(marks[0], 8000, 8000, 100)
    took = int(marks[1]["t_ms"]) - int(marks[0]["t_ms"])
    assert call_floor(200) <= took < 1000, took
    assert events(vm, "pcm_stop")[-1]["busy"] == "0", vm.serial

    # Tone, replaced by PCM, replaced by a tone, all with AUDIO_PRIORITY.
    # Each replacement prints a stop line first.
    marks = c["pcm_swap"]
    assert [m["op"] for m in marks] == ["tone", "stop", "pcm", "stop", "tone", "stop"], marks
    assert (marks[0]["hz"], marks[4]["hz"]) == ("440", "880"), marks
    assert marks[1]["t_ms"] == marks[2]["t_ms"], marks
    check_pcm_start(marks[2], 8000, 8000, 100)
    assert int(marks[4]["t_ms"]) - int(marks[2]["t_ms"]) < 1000, marks
    swap = events(vm, "pcm_swap")[-1]
    assert (int(swap["busy"]), int(swap["prio"])) == (ERR_BUSY, 0), swap
    assert (int(swap["tbusy"]), int(swap["tprio"])) == (ERR_BUSY, 0), swap
    check_busy_big(swap)

    # Each cut prints its stop line at the tick; the replacements then start.
    marks = c["pcm_cut"]
    assert [m["op"] for m in marks] == ["pcm", "stop", "pcm", "stop", "tone", "stop",
                                        "pcm", "stop", "pcm", "stop"], marks
    assert marks[4]["hz"] == "440", marks
    check_pcm_cut(vm)

    # 25 ms looped: still playing after 300 ms, ends only on the stop.
    marks = c["pcm_loop"]
    assert [m["op"] for m in marks] == ["pcm", "stop"], marks
    check_pcm_start(marks[0], 8000, 200, 100)
    assert int(marks[1]["t_ms"]) - int(marks[0]["t_ms"]) >= call_floor(300), marks
    assert events(vm, "pcm_loop")[-1]["busy"] == "1", vm.serial

    # One second at 22050 Hz: the tick keeps counting and running its callbacks.
    check_pcm_play(c["pcm_tick"], 22050, 22050, 100)
    tick = events(vm, "pcm_tick")[-1]
    expected = int(tick["us"]) / (TICK_MS * 1000)
    # tick.c catches up: a late IRQ counts every elapsed period, so the count
    # never runs ahead of time. It lags only while a tick IRQ is still pending
    # when the example reads it, which a host stall can stretch. 90 % still
    # fails if the tick starves during PCM.
    ticks = int(tick["ticks"])
    assert 0.9 * expected <= ticks <= expected + 2, tick
    assert int(tick["runs"]) >= 0.9 * int(tick["ticks"]), tick

    assert c["pcm_args"] == [], c["pcm_args"]
    check_pcm_args(vm)

    check_pcm_chain(c["pcm_chain"])
    check_pcm_chain_results(vm)

    # Queue errors around a looped play: one start, one stop, nothing chained.
    marks = c["pcm_qerr"]
    assert [m["op"] for m in marks] == ["pcm", "stop"], marks
    check_pcm_qerr(vm)

    # A stop, then a tone, each with a buffer waiting: no pcm_next, no pcm_end.
    marks = c["pcm_qcut"]
    assert [m["op"] for m in marks] == ["pcm", "stop", "pcm", "stop", "tone", "stop"], marks
    assert marks[4]["hz"] == "440", marks
    check_pcm_qcut(vm)

    # Underrun: the first play ends alone, then a new play starts.
    marks = c["pcm_under"]
    assert [m["op"] for m in marks] == ["pcm", "pcm_end", "pcm", "pcm_end"], marks
    check_pcm_under(vm)

    check_pcm_mix_trace(c["pcm_mix"])
    check_pcm_mix(vm)

    check_pcm_voice_trace(c["pcm_voice"])
    check_pcm_voice(vm)


def check_busy_big(swap):
    """A busy audio_play_pcm() returns before it scans its 16 MB buffer."""
    assert int(swap["big"]) == ERR_BUSY, swap
    # The busy check alone takes microseconds; the margin absorbs a host stall.
    assert int(swap["big_us"]) < BUSY_BIG_MAX_US, swap


def check_pcm_cut(vm):
    """No sample IRQ reads the old buffer once audio_stop() or a replacement returns."""
    cuts = {m["kind"]: m for m in events(vm, "pcm_cut")}
    assert set(cuts) == {"stop", "tone", "pcm"}, vm.serial
    for m in cuts.values():
        assert (m["ret"], m["cut"]) == ("0", "0"), m
        # The IRQ ran before the cut, so a late read would show.
        assert int(m["before"]) > 0, m
        assert m["after"] == "0", m


def check_pcm_chain(marks):
    """Three chunks back to back: one start, two switches in order, one end."""
    assert [m["op"] for m in marks] == ["pcm", "pcm_next", "pcm_next", "pcm_end"], marks
    data = pcm_data(PCM_BUF)
    (a_off, a_len), (b_off, b_len), (c_off, c_len) = CHAIN
    start, nb, nc, end = marks
    assert (int(start["rate"]), int(start["len"]), int(start["vol"])) == (8000, a_len, 100), start
    assert start["crc"] == "%08x" % zlib.crc32(data[a_off:a_off + a_len]), start
    for m, (off, n) in ((nb, CHAIN[1]), (nc, CHAIN[2])):
        assert int(m["len"]) == n, m
        assert m["crc"] == "%08x" % zlib.crc32(data[off:off + n]), m
    total = a_len + b_len + c_len
    t0 = int(start["t_ms"])
    # Each switch is due once the earlier chunks have played.
    assert int(nb["t_ms"]) - t0 >= a_len * 1000 // 8000, marks
    assert int(nc["t_ms"]) - t0 >= (a_len + b_len) * 1000 // 8000, marks
    # The whole schedule: never shorter than the total length over the rate.
    nominal = total * 1000 // 8000
    took = int(end["t_ms"]) - t0
    assert nominal <= took < nominal + PCM_END_SLACK_MS, (took, nominal)
    # n counts over the three chunks; last is an index in the final one.
    assert nominal // 2 <= int(end["n"]) <= total, end
    last = int(end["last"])
    assert max(0, c_len - 1 - 8000 * PCM_LAST_SLACK_MS // 1000) <= last <= c_len - 1, end


def check_pcm_chain_results(vm):
    m = events(vm, "pcm_chain")[-1]
    got = {k: int(v) for k, v in m.items() if k not in ("stage", "ev")}
    assert got == {"can0": 1, "q1": 0, "can1": 0, "qbusy": ERR_BUSY, "can2": 1, "q2": 0,
                   "can3": 0, "can4": 0, "under": ERR_STATE, "idle": 0}, m


def check_pcm_qerr(vm):
    m = events(vm, "pcm_qerr")[-1]
    got = {k: int(v) for k, v in m.items() if k not in ("stage", "ev")}
    assert got == {"idle": ERR_STATE, "null": ERR_ARG, "zero": ERR_ARG, "loop": ERR_STATE,
                   "lcan": 0}, m


def check_pcm_qcut(vm):
    """No sample IRQ reads either buffer once audio_stop() or a tone returns."""
    cuts = {m["kind"]: m for m in events(vm, "pcm_qcut")}
    assert set(cuts) == {"stop", "tone"}, vm.serial
    for m in cuts.values():
        assert (m["ret"], m["q"], m["cut"], m["can"]) == ("0", "0", "0", "0"), m
        assert int(m["before"]) > 0, m
        assert m["after"] == "0", m


def check_pcm_under(vm):
    m = events(vm, "pcm_under")[-1]
    assert (int(m["under"]), int(m["can"]), int(m["again"])) == (ERR_STATE, 0, 0), m


def check_pcm_mix_trace(marks):
    """A mix on a play that waits, then one longer than what is left of a play."""
    assert [m["op"] for m in marks] == ["pcm", "pcm_mix", "pcm_end",
                                        "pcm", "pcm_mix", "pcm_end"], marks
    data = pcm_data(PCM_BUF)
    for start, mix, end, (off, n) in ((marks[0], marks[1], marks[2], (2000, 200)),
                                      (marks[3], marks[4], marks[5], (4000, 2000))):
        check_pcm_start(start, 8000, 800, 100)
        assert int(mix["len"]) == n, mix
        assert mix["crc"] == "%08x" % zlib.crc32(data[off:off + n]), mix
        # The overlay never makes the play longer.
        took = int(end["t_ms"]) - int(start["t_ms"])
        assert 100 <= took < 100 + PCM_END_SLACK_MS, (took, marks)
    # A play that waits for its tick starts with its overlay.
    assert marks[1]["t_ms"] == marks[0]["t_ms"], marks
    # The second overlay joins 50 ms in, before the play ends.
    gap = int(marks[4]["t_ms"]) - int(marks[3]["t_ms"])
    assert call_floor(50) <= gap <= int(marks[5]["t_ms"]) - int(marks[3]["t_ms"]), marks


def check_pcm_mix(vm):
    m = events(vm, "pcm_mix")[-1]
    got = {k: int(v) for k, v in m.items() if k not in ("stage", "ev")}
    assert got == {"idle": ERR_STATE, "mix0": 0, "busy0": 1, "again": ERR_BUSY,
                   "null": ERR_ARG, "ret2": 0, "mix1": 0, "live": 1, "done": 0}, m


# The voice of examples/audio/voice_demo.h, in the same integer maths.
VOICE_RATE, VOICE_FADE_N, VOICE_GLIDE_N = 37500, 1875, 3750
VOICE_INC, VOICE_DINC, VOICE_DAMP, VOICE_P2 = 114532461, 30542, 1398, 8192
VOICE_MIX_INC, VOICE_MIX_DELAY, VOICE_MIX_BURST, VOICE_MIX_PEAK = 57266230, 3000, 1500, 30
U32 = 0xFFFFFFFF


def voice_spans():
    """The three spans as (n, phase, inc, dinc, amp, damp, p2); each carries the last."""
    def carry(s):
        n, phase, inc, dinc, amp, damp, _ = s
        return ((phase + n * inc + dinc * (n * (n - 1) // 2)) & U32, (inc + n * dinc) & U32,
                amp + n * damp)

    s0 = (VOICE_FADE_N, 0, VOICE_INC, 0, 0, VOICE_DAMP, VOICE_P2)
    ph, inc, amp = carry(s0)
    s1 = (VOICE_GLIDE_N, ph, inc, VOICE_DINC, amp, 0, VOICE_P2)
    ph, inc, amp = carry(s1)
    s2 = (VOICE_FADE_N, ph, inc, 0, amp, -VOICE_DAMP, VOICE_P2)
    return [s0, s1, s2]


def voice_bytes():
    """The span array as the library sees it: 28 bytes each, little-endian."""
    return b"".join(struct.pack("<IIIiiiI", *s) for s in voice_spans())


def audio_sine():
    """src/lib/audio_voice.c audio_sine[]: floor(32767 * sin(2 pi k / 1024) + 0.5)."""
    table = [math.floor(32767 * math.sin(2 * math.pi * k / 1024) + 0.5) for k in range(1024)]
    assert zlib.crc32(struct.pack("<1024h", *table)) == 0x05067686
    return table


def voice_mix():
    """voice_demo_mix(): 128 for the delay, then the burst on a triangle envelope."""
    sine = audio_sine()
    out = bytearray([128] * VOICE_MIX_DELAY)
    den = (VOICE_MIX_BURST - 1) * 32768
    for k in range(VOICE_MIX_BURST):
        s = sine[((k * VOICE_MIX_INC) & U32) >> 22] * min(k, VOICE_MIX_BURST - 1 - k)
        num = s * 2 * VOICE_MIX_PEAK
        v = abs(num) // den * (1 if num >= 0 else -1)   # C division truncates toward 0
        out.append(128 + v)
    return bytes(out)


def check_pcm_voice_trace(marks):
    """op=voice, its overlay on the same t_ms, then the end after 200 ms."""
    assert [m["op"] for m in marks] == ["voice", "pcm_mix", "pcm_end"], marks
    start, mix, end = marks
    spans = voice_spans()
    length = sum(s[0] for s in spans)
    assert length == 7500
    assert (int(start["rate"]), int(start["spans"]), int(start["len"]), int(start["vol"])) == \
        (VOICE_RATE, len(spans), length, 100), start
    assert start["crc"] == "%08x" % zlib.crc32(voice_bytes()), start
    mix_data = voice_mix()
    assert int(mix["len"]) == len(mix_data), mix
    assert mix["crc"] == "%08x" % zlib.crc32(mix_data), mix
    assert mix["t_ms"] == start["t_ms"], marks
    nominal = length * 1000 // 37500
    took = int(end["t_ms"]) - int(start["t_ms"])
    assert nominal <= took < nominal + PCM_END_SLACK_MS, (took, nominal)
    assert nominal // 2 <= int(end["n"]) <= length, end
    last = int(end["last"])
    assert max(0, length - 1 - 37500 * PCM_LAST_SLACK_MS // 1000) <= last <= length - 1, end


def check_pcm_voice(vm):
    m = events(vm, "pcm_voice")[-1]
    got = {k: int(v) for k, v in m.items() if k not in ("stage", "ev")}
    assert got == {"loop": ERR_ARG, "none": ERR_ARG, "mix": 0, "queue": ERR_STATE, "can": 0,
                   "busy": ERR_BUSY}, m


def check_rest(vm, marks):
    """The ramp as a plain play at vol=100, then a tone that still sounds."""
    ops = [m["op"] for m in marks]
    assert ops == ["pcm", "pcm_end", "tone", "stop"], marks
    ramp, end, tone, stop = marks
    assert (int(ramp["rate"]), int(ramp["len"]), int(ramp["vol"])) == \
        (RAMP_RATE, RAMP_LEN, 100), ramp
    assert ramp["crc"] == "%08x" % zlib.crc32(ramp_data()), ramp
    took = int(end["t_ms"]) - int(ramp["t_ms"])
    assert 40 <= took < 40 + PCM_END_SLACK_MS, (took, marks)
    assert (tone["hz"], tone["vol"]) == ("440", "50"), tone
    assert int(stop["t_ms"]) - int(tone["t_ms"]) >= 50, marks
    m = events(vm, "rest")[-1]
    assert (int(m["again"]), int(m["tone"])) == (0, 0), m


def check_pcm_args(vm):
    args = events(vm, "pcm_args")[-1]
    assert {k: int(args[k]) for k in ("low", "high", "null", "zero", "flags")} == {
        "low": ERR_RANGE, "high": ERR_RANGE, "null": ERR_ARG, "zero": ERR_ARG,
        "flags": ERR_ARG}, args


def check_ops(marks):
    for m in marks:
        assert m["op"] in ("tone", "stop"), m
        assert (m["op"] == "stop") == (m["hz"] == "0"), m


def check_durations(got, expected):
    """Each step lasts at least its nominal time by the guest clock.

    The sequencer stamps a step's end with its due time, start stamp plus
    length, from the same audio clock, so neither tick latency nor
    truncation shortens a step. A late tick only makes a stamp later.
    """
    for (hz, t), (_, t_next), (_, nominal) in zip(got, got[1:], expected):
        assert t_next - t >= nominal, (hz, t, t_next, nominal)


def check_exact(marks, expected):
    check_ops(marks)
    got = tones(marks)
    assert [hz for hz, _ in got] == [hz for hz, _ in expected], got
    check_durations(got, expected)


def check_trace(vm):
    c = cases(vm)
    assert set(c) == set(ALL_CASES), vm.serial
    for m in events(vm, "case_end"):
        assert m["ret"] == "0", vm.serial

    check_exact(c["beep"], BEEP)
    check_exact(c["seq"], SEQ)
    check_exact(c["tune"], TUNE)
    assert all(m["vol"] == "60" for m in c["tune"]), vm.serial
    assert all(m["vol"] == "100" for m in c["seq"]), vm.serial

    # Loop: the pattern repeats from the start until stopped, then a final stop.
    marks = c["loop"]
    check_ops(marks)
    got = tones(marks)
    hz = [h for h, _ in got]
    assert hz[-1] == 0, got
    body = hz[:-1]
    pattern = [h for h, _ in LOOP]
    assert len(body) >= 2 * len(pattern), got
    for i, h in enumerate(body):
        assert h == pattern[i % len(pattern)], got
    check_durations(got[:-1], LOOP * (len(body) // len(LOOP) + 1))
    # The example stops after 700 ms of guest time.
    assert got[-1][1] - got[0][1] >= call_floor(700), got

    # Interrupt: 440 Hz for 3 s is cut short by 880 Hz with AUDIO_PRIORITY.
    # The cut prints a stop line on the same tick as the new start.
    marks = c["interrupt"]
    check_ops(marks)
    got = tones(marks)
    assert [h for h, _ in got] == [440, 0, 880, 0], got
    assert got[1][1] == got[2][1], got
    assert got[2][1] - got[0][1] >= call_floor(200), got
    assert got[2][1] - got[0][1] < 3000, got
    # The sequencer ends the 880 Hz note: its stop stamp is the start stamp + 300.
    assert got[3][1] - got[2][1] >= 300, got
    inter = events(vm, "interrupt")[-1]
    assert inter["busy"] == "-8" and inter["prio"] == "0", vm.serial

    check_pcm(vm, c)
    check_rule(vm)
    check_rest(vm, c["rest"])
    assert not events(vm, "audio_drop"), vm.serial


def runs(vm, name):
    """The audio markers of every run of case `name`, in order."""
    out = []
    cur = None
    for m in vm.markers():
        if m.get("stage") != "audio":
            continue
        if m.get("ev") == "case":
            cur = [] if m["name"] == name else None
        elif m.get("ev") == "case_end" and cur is not None:
            out.append(cur)
            cur = None
        elif m.get("ev") == "audio" and cur is not None:
            cur.append(m)
    return out


def shape(marks):
    """A trace without its time stamps and counts: what the rule fixes."""
    keep = ("op", "hz", "rate", "len", "crc")
    return [tuple((k, m[k]) for k in keep if k in m) for m in marks]


def check_rule(vm):
    """Stop then PCM in one tick, and a stop in a rest: one stop line, the same every run."""
    got = runs(vm, "stop_pcm")
    assert len(got) == RULE_RUNS, vm.serial
    for marks in got:
        assert [m["op"] for m in marks] == ["tone", "stop", "pcm", "pcm_end"], marks
        assert marks[0]["hz"] == "440", marks
        check_pcm_start(marks[2], 8000, 800, 100)
    assert all(shape(m) == shape(got[0]) for m in got), got

    got = runs(vm, "stop_rest")
    assert len(got) == RULE_RUNS, vm.serial
    for marks in got:
        # The tone, the rest, then the stop the call made in the rest.
        assert [(m["op"], m["hz"]) for m in marks] == \
            [("tone", "440"), ("stop", "0"), ("stop", "0")], marks
        assert int(marks[2]["t_ms"]) - int(marks[1]["t_ms"]) < 500, marks
    assert all(shape(m) == shape(got[0]) for m in got), got


def check_common(vm):
    order = events(vm, "order")
    assert len(order) == 1, vm.serial
    # AUDIO_ERR_NO_IRQ, then AUDIO_ERR_NO_INIT.
    assert (order[0]["init"], order[0]["tone"]) == ("-2", "-1"), vm.serial
    assert int(order[0]["pcm"]) == ERR_NO_INIT, vm.serial
    assert not events(vm, "exception"), vm.serial


def test_audio_trace(board, example_image):
    with boot(example_image("audio"), board) as vm:
        vm.wait_for(DONE, timeout=60)
        check_common(vm)
        backend = events(vm, "backend")
        assert len(backend) == 1, vm.serial
        assert backend[0]["name"] == "trace", vm.serial
        assert backend[0]["probe_pwm"] == PROBE[board], vm.serial
        assert backend[0]["err"] == "0", vm.serial
        check_trace(vm)

        # Replay one case from the UART, as a listening test would.
        vm.send("s")
        vm.wait_for(REPLAYED, timeout=30)
        check_exact(cases(vm)["seq"], SEQ)
        vm.send("p")
        vm.wait_for(re.compile(r"ev=ready .*\n(?:.*\n)*?.*ev=case_end name=seq .*\n"
                               r"(?:.*\n)*?.*ev=case_end name=pcm .*\n"),
                    timeout=30)
        check_pcm_play(cases(vm)["pcm"], 8000, 800, 100)
        assert not events(vm, "exception"), vm.serial


def test_audio_forced_pwm(board, example_image):
    """Forcing PWM must not crash where QEMU ignores it and must fall back where it aborts."""
    with boot(example_image("audio", "pwm", "-DROBCO_AUDIO_BACKEND=1"), board) as vm:
        vm.wait_for(DONE, timeout=60)
        check_common(vm)
        backend = events(vm, "backend")[0]
        assert backend["probe_pwm"] == PROBE[board], vm.serial
        if PROBE[board] == "abort":
            # AUDIO_ERR_NO_PWM, and audio still works on the trace backend.
            assert (backend["name"], backend["err"]) == ("trace", "-5"), vm.serial
            check_trace(vm)
        else:
            assert (backend["name"], backend["err"]) == ("pwm", "0"), vm.serial
            assert not events(vm, "audio"), vm.serial
            ends = events(vm, "case_end")
            assert [m["name"] for m in ends] == ALL_CASES, vm.serial
            assert all(m["ret"] == "0" for m in ends), vm.serial
            # The PCM engine runs on PWM too: stop, swap, loop and arguments behave the same.
            assert events(vm, "pcm_stop")[-1]["busy"] == "0", vm.serial
            swap = events(vm, "pcm_swap")[-1]
            assert [int(swap[k]) for k in ("busy", "prio", "tbusy", "tprio")] == \
                [ERR_BUSY, 0, ERR_BUSY, 0], swap
            check_busy_big(swap)
            assert events(vm, "pcm_loop")[-1]["busy"] == "1", vm.serial
            check_pcm_cut(vm)
            tick = events(vm, "pcm_tick")[-1]
            # audio_wait() returns after one second: the IRQ ended the play.
            assert 1000000 <= int(tick["us"]) < 1000000 + PCM_END_SLACK_MS * 1000, tick
            check_pcm_args(vm)
            check_pcm_chain_results(vm)
            check_pcm_qerr(vm)
            check_pcm_qcut(vm)
            check_pcm_under(vm)
            check_pcm_mix(vm)
            check_pcm_voice(vm)
            m = events(vm, "rest")[-1]
            assert (int(m["again"]), int(m["tone"])) == (0, 0), m
