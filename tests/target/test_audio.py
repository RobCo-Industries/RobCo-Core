"""Boot examples/audio through boot() on every board: backend choice and trace markers."""

import re

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
    assert set(c) == {"beep", "seq", "tune", "loop", "interrupt"}, vm.serial
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

    assert not events(vm, "audio_drop"), vm.serial


def check_common(vm):
    order = events(vm, "order")
    assert len(order) == 1, vm.serial
    # AUDIO_ERR_NO_IRQ, then AUDIO_ERR_NO_INIT.
    assert (order[0]["init"], order[0]["tone"]) == ("-2", "-1"), vm.serial
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
            assert [m["name"] for m in ends] == ["beep", "seq", "tune", "loop", "interrupt"]
            assert all(m["ret"] == "0" for m in ends), vm.serial
