"""Boot examples/input with real GPIO levels driven through qtest (canary V18).

Every active-low line starts high, so the stuck-line guard passes. The
guest samples on its 100 Hz tick (10 ms) with a 20 ms debounce. Host
timing jitter on a loaded machine reached 9 ms, so each timing allowance
below names its margin.
"""

import re
import time

import pytest

from harness.boot import boot
from harness.pigpio import PiGpio, bounce_schedule, slip

PINS = {"up": 5, "down": 6, "left": 17, "right": 22, "select": 26, "back": 27,
        "dial_push": 16}
DIAL_A, DIAL_B = 23, 24
ALL_PINS = list(PINS.values()) + [DIAL_A, DIAL_B]
TIMEOUT = 60

TICK_S = 0.010
# A press held 10 samples plus the host jitter is debounced for sure.
HOLD_S = 0.15
# A press shorter than the debounce time (20 ms, 2 samples) gives nothing.
# Two samples see a pulse only if it outlasts the shortest sample gap,
# TICK_S minus the tick's jitter: a pulse that ran longer than that on the
# host is tried again, so the check never depends on luck.
GLITCH_S = 0.004
# Slow dial: 3 samples per state even with 9 ms of jitter.
SLOW_STATE_S = 0.030
# A state this short is hidden between two samples most of the time.
HIDDEN_S = 0.002
# The enforced limit: 15 ms per state. Equal states give 16.7 detents per
# second, 3:1 states (the rest three times longer) 11.1. Sweeps on all four
# boards counted every step down to 12 ms and lost steps at 10 ms, so the
# margin is 3 ms per state.
LIMIT_STATE_S = 0.015
SETTLE_S = 0.1
# The decoder is exact while a sample gap hides at most one state and two
# gaps in a row never both hide one. On the tick's fixed grid that holds
# while the spread of the tick's lateness stays under 2 * (state - short)
# - 2 * TICK_S, where short is how much the host cut a state. A run that
# misses a tick or breaks this bound is the machine's fault: it is tried
# again, then skipped with its numbers. A step lost in any other run fails.
JITTER_TRIES = 3


def timing_ok(short_s, missed, jitter_s, state_s=LIMIT_STATE_S):
    return missed == 0 and jitter_s < 2 * (state_s - short_s) - 2 * TICK_S


def markers(vm, name):
    return [m for m in vm.markers() if m.get("stage") == "input" and m.get("ev") == name]


def gpio_events(vm):
    return [(m["type"], m["code"], int(m["value"]))
            for m in markers(vm, "event") if m["src"] == "gpio"]


def total(vm):
    reply = vm.script("totals")
    return int(re.search(r"dial0=(-?\d+)", " ".join(reply.lines)).group(1))


def ticks(vm):
    """(missed ticks, lateness spread in s) since the last call."""
    m = re.search(r"missed=(\d+) maxgap=\d+ jitter=(\d+)", " ".join(vm.script("ticks").lines))
    return int(m.group(1)), int(m.group(2)) / 1e6


def start(vm, status="active"):
    vm.wait_for(re.compile(r"@RC stage=input ev=ready\n"), timeout=TIMEOUT)
    vm.wait_for("ready\n", timeout=TIMEOUT, port=1)
    gpio = markers(vm, "gpio")
    assert len(gpio) == 1 and gpio[0]["status"] == status, vm.serial
    return PiGpio(vm, vm_board(vm), names=PINS)


def vm_board(vm):
    return vm.qmp.machine


def wait_events(vm, n, timeout=5.0):
    deadline = time.monotonic() + timeout
    while len(gpio_events(vm)) < n and time.monotonic() < deadline:
        time.sleep(0.02)
    time.sleep(SETTLE_S)  # nothing more may come
    return gpio_events(vm)


def short_pulses(vm, io, schedule, longest_s, excused):
    """Plays presses planned at most longest_s long, each seen by one sample at most.

    A late write stretches a press by up to slip()'s latest write. Two
    samples can be TICK_S minus the tick's jitter apart, so a run whose
    press may have lasted that long is played again, then skipped. The
    events such a run gave go into the set `excused`, by index.
    """
    for _ in range(JITTER_TRIES):
        n = len(gpio_events(vm))
        ticks(vm)
        io.play(schedule)
        late, _ = slip(io.log)
        missed, jitter = ticks(vm)
        if missed == 0 and longest_s + late < TICK_S - jitter:
            return
        time.sleep(SETTLE_S)
        excused.update(range(n, len(gpio_events(vm))))
    pytest.skip(f"machine too busy: a write {late * 1000:.1f} ms late, "
                f"tick jitter {jitter * 1000:.1f} ms")


def no_exception(vm):
    assert not [m for m in vm.markers() if m.get("ev") == "exception"], vm.serial


def test_gpio_buttons(board, example_image):
    """Each button gives one press and one release; a glitch gives nothing."""
    with boot(example_image("input"), board, serial2=True, qtest=True,
              gpio_high=ALL_PINS) as vm:
        io = start(vm)
        want = []
        for name in PINS:
            io.tap(name, HOLD_S)
            time.sleep(HOLD_S)
            want += [("key", name, 1), ("key", name, 0)]
            assert wait_events(vm, len(want)) == want, vm.serial
        sel, excused = PINS["select"], set()
        for _ in range(5):
            short_pulses(vm, io, [(0, sel, 0), (GLITCH_S, sel, 1)], GLITCH_S, excused)
            time.sleep(3 * TICK_S)
        # Chatter, 2 ms pressed each time, ends released.
        short_pulses(vm, io, bounce_schedule(PINS["back"], True, 5, GLITCH_S, False),
                     GLITCH_S / 2, excused)
        time.sleep(SETTLE_S)
        got = [e for i, e in enumerate(gpio_events(vm)) if i not in excused]
        assert got == want, vm.serial
        no_exception(vm)


def test_gpio_repeat(board, example_image):
    """A held key repeats after 300 ms, then every 100 ms (the repeat variant)."""
    img = example_image("input", "repeat", "-DINPUT_DEMO_REPEAT=1")
    with boot(img, board, serial2=True, qtest=True, gpio_high=ALL_PINS) as vm:
        io = start(vm)
        io.tap("up", 1.0)
        ev = wait_events(vm, 2)
        assert ev[0] == ("key", "up", 1) and ev[-1] == ("key", "up", 0), vm.serial
        reps = [e for e in ev if e == ("key", "up", 2)]
        # 1000 ms held: (1000 - 300) / 100 = 7 repeats, one less or more for tick and host timing.
        assert 6 <= len(reps) <= 8 and len(ev) == len(reps) + 2, vm.serial


def test_gpio_all_low_disables(board, example_image):
    """Undriven lines read 0: the guard turns GPIO off, the console still works."""
    with boot(example_image("input"), board, serial2=True, qtest=True) as vm:
        io = start(vm, "disabled_stuck")
        for name in PINS:
            io.release(name)
        io.tap("select", HOLD_S)
        vm.send("z")
        vm.wait_for(re.compile(r"src=serial code=122 "), timeout=10)
        assert total(vm) == 0          # the script channel answers too
        time.sleep(SETTLE_S)
        assert gpio_events(vm) == [], vm.serial


def test_gpio_dial_slow(board, example_image):
    """One event per detent with the right sign, then the push button."""
    with boot(example_image("input"), board, serial2=True, qtest=True,
              gpio_high=ALL_PINS) as vm:
        io = start(vm)
        io.turn(DIAL_A, DIAL_B, 3, SLOW_STATE_S)
        io.turn(DIAL_A, DIAL_B, -2, SLOW_STATE_S)
        want = [("dial", "0", 1)] * 3 + [("dial", "0", -1)] * 2
        assert wait_events(vm, 5) == want, vm.serial
        io.tap("dial_push", HOLD_S)
        assert wait_events(vm, 7)[5:] == [("key", "dial_push", 1), ("key", "dial_push", 0)]
        assert total(vm) == 1


def test_gpio_dial_bounce(board, example_image):
    """Contact bounce on channel A, 3 flips of 1 ms per edge, adds nothing."""
    with boot(example_image("input"), board, serial2=True, qtest=True,
              gpio_high=ALL_PINS) as vm:
        io = start(vm)
        io.turn(DIAL_A, DIAL_B, 4, SLOW_STATE_S, bounce=(DIAL_A, 3, 0.001))
        io.turn(DIAL_A, DIAL_B, -4, SLOW_STATE_S, bounce=(DIAL_A, 3, 0.001))
        want = [("dial", "0", 1)] * 4 + [("dial", "0", -1)] * 4
        assert wait_events(vm, 8) == want, vm.serial
        assert total(vm) == 0


def test_gpio_dial_hidden_states(board, example_image):
    """States of 2 ms between states of 30 ms: the decoder infers the ones it misses.

    A 2 ms state falls between two samples most of the time. Its 30 ms
    neighbours are each seen, so at most one state hides per gap. The
    short state is either the first after the detent, which also starts
    each turn back, or the middle one of the detent.
    """
    h = HIDDEN_S / SLOW_STATE_S
    # Gray positions are entered 1, 2, 3, 0 clockwise and 3, 2, 1, 0 back.
    runs = [(4, (1, h, 1, 1)), (4, (1, 1, h, 1)), (-4, (1, 1, 1, h)), (-4, (1, 1, h, 1)),
            (4, (1, h, 1, 1)), (-4, (1, 1, 1, h))]
    with boot(example_image("input"), board, serial2=True, qtest=True,
              gpio_high=ALL_PINS) as vm:
        io = start(vm)
        for detents, lengths in runs:
            t0 = total(vm)
            io.turn(DIAL_A, DIAL_B, detents, SLOW_STATE_S, lengths)
            time.sleep(SETTLE_S)
            assert total(vm) - t0 == detents, (detents, lengths, vm.serial)
        no_exception(vm)


def test_gpio_dial_at_the_limit(board, example_image):
    """20 detents each way at the stated limit, equal and 3:1 unequal states, lose nothing.

    Equal: 15 ms per state, 16.7 detents per second. Unequal: the rest
    state lasts three times longer, 11 detents per second. The old decoder
    lost steps at these rates in some runs.
    The decoder infers one missed state, so a sample may land late by up
    to one state. After each run a half-detent wiggle at rest proves the
    phase did not shift.
    """
    with boot(example_image("input"), board, serial2=True, qtest=True,
              gpio_high=ALL_PINS) as vm:
        io = start(vm)
        for lengths in ((1, 1, 1, 1), (3, 1, 1, 1)):
            for detents in (20, -20):
                for _ in range(JITTER_TRIES):
                    t0 = total(vm)
                    m0, _ = ticks(vm)
                    io.turn(DIAL_A, DIAL_B, detents, LIMIT_STATE_S, lengths)
                    _, short = slip(io.log)
                    missed, jitter = ticks(vm)
                    missed -= m0
                    time.sleep(SETTLE_S)
                    got = total(vm) - t0
                    if timing_ok(short, missed, jitter):
                        break
                else:
                    pytest.skip(f"machine too busy: a state cut by {short * 1000:.1f} ms, "
                                f"{missed} ticks missed, tick jitter {jitter * 1000:.1f} ms")
                assert got == detents, (lengths, short, missed, jitter, vm.serial)
        n = len(gpio_events(vm))
        # Two transitions out and back: a correct phase gives no event.
        io.play([(0, DIAL_A, 0), (SLOW_STATE_S, DIAL_B, 0), (2 * SLOW_STATE_S, DIAL_B, 1),
                 (3 * SLOW_STATE_S, DIAL_A, 1), (4 * SLOW_STATE_S, None, None)])
        time.sleep(SETTLE_S)
        assert len(gpio_events(vm)) == n, vm.serial
        no_exception(vm)
