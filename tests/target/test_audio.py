"""Boot examples/audio through boot() on every board: backend choice and trace markers."""

import re
import zlib

from harness.boot import boot

DONE = re.compile(r"@RC stage=audio ev=done\n")
REPLAYED = re.compile(r"ev=done\n(?:.*\n)*?.*ev=case_end name=seq .*\n")
TICK_MS = 10  # the example runs a 100 Hz tick

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
PCM_CASES = ["pcm", "pcm_fast", "pcm_stop", "pcm_swap", "pcm_cut", "pcm_loop", "pcm_tick", "pcm_args"]

# AUDIO_ERR_* codes.
ERR_NO_INIT, ERR_RANGE, ERR_ARG, ERR_BUSY = -1, -6, -7, -8

# A late end is fine under load; this only catches a play that never ends on time.
PCM_END_SLACK_MS = 1000
# A late IRQ skips samples, and the host can stall the guest for some ms near the end.
# Under load the last sample landed 3 to 4 ms before the end.
PCM_LAST_SLACK_MS = 20
# A busy call with a 16 MB buffer: a CRC over it took 89 to 119 ms in QEMU.
BUSY_BIG_MAX_US = 30000


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

    # Stopped after 200 ms of a 1 s play: a stop line, no pcm_end.
    marks = c["pcm_stop"]
    assert [m["op"] for m in marks] == ["pcm", "stop"], marks
    check_pcm_start(marks[0], 8000, 8000, 100)
    took = int(marks[1]["t_ms"]) - int(marks[0]["t_ms"])
    assert 200 - TICK_MS <= took < 1000, took
    assert events(vm, "pcm_stop")[-1]["busy"] == "0", vm.serial

    # Tone, replaced by PCM, replaced by a tone, all with AUDIO_PRIORITY.
    marks = c["pcm_swap"]
    assert [m["op"] for m in marks] == ["tone", "pcm", "stop", "tone", "stop"], marks
    assert (marks[0]["hz"], marks[3]["hz"]) == ("440", "880"), marks
    check_pcm_start(marks[1], 8000, 8000, 100)
    assert int(marks[3]["t_ms"]) - int(marks[1]["t_ms"]) < 1000, marks
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
    assert int(marks[1]["t_ms"]) - int(marks[0]["t_ms"]) >= 300 - TICK_MS, marks
    assert events(vm, "pcm_loop")[-1]["busy"] == "1", vm.serial

    # One second at 22050 Hz: the tick keeps counting and running its callbacks.
    check_pcm_play(c["pcm_tick"], 22050, 22050, 100)
    tick = events(vm, "pcm_tick")[-1]
    expected = int(tick["us"]) / (TICK_MS * 1000)
    assert abs(int(tick["ticks"]) - expected) <= 2, tick
    assert int(tick["runs"]) >= 0.9 * int(tick["ticks"]), tick

    assert c["pcm_args"] == [], c["pcm_args"]
    check_pcm_args(vm)


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
    """Each step lasts at least its nominal time by the guest clock, minus one tick."""
    for (hz, t), (_, t_next), (_, nominal) in zip(got, got[1:], expected):
        assert t_next - t >= nominal - TICK_MS, (hz, t, t_next, nominal)


def check_exact(marks, expected):
    check_ops(marks)
    got = tones(marks)
    assert [hz for hz, _ in got] == [hz for hz, _ in expected], got
    check_durations(got, expected)


def check_trace(vm):
    c = cases(vm)
    assert set(c) == set(TONE_CASES + PCM_CASES), vm.serial
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
    assert got[-1][1] - got[0][1] >= 700 - TICK_MS, got

    # Interrupt: 440 Hz for 3 s is cut short by 880 Hz with AUDIO_PRIORITY.
    marks = c["interrupt"]
    check_ops(marks)
    got = tones(marks)
    assert [h for h, _ in got] == [440, 880, 0], got
    assert got[1][1] - got[0][1] >= 200 - TICK_MS, got
    assert got[1][1] - got[0][1] < 3000, got
    assert got[2][1] - got[1][1] >= 300 - TICK_MS, got
    inter = events(vm, "interrupt")[-1]
    assert inter["busy"] == "-8" and inter["prio"] == "0", vm.serial

    check_pcm(vm, c)
    assert not events(vm, "audio_drop"), vm.serial


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
            assert [m["name"] for m in ends] == TONE_CASES + PCM_CASES, vm.serial
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
