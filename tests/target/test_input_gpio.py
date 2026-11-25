"""Boot examples/input with real GPIO levels driven through qtest (canary V19).

Every active-low line starts high, so the stuck-line guard passes. The
guest samples on its 100 Hz tick (10 ms) with a 20 ms debounce. Host
timing jitter on a loaded machine reached 9 ms, so each timing allowance
below names its margin.
"""

import re
import time

from harness import hosttiming
from harness.boot import boot
from harness.pigpio import PiGpio, bounce_schedule, one_sample_at_most, pressed_span, slip

PINS = {"up": 5, "down": 6, "left": 17, "right": 22, "select": 26, "back": 27,
        "dial_push": 16}
DIAL_A, DIAL_B = 23, 24
ALL_PINS = list(PINS.values()) + [DIAL_A, DIAL_B]
TIMEOUT = 60

TICK_S = 0.010
# A press held 10 samples plus the host jitter is debounced for sure.
HOLD_S = 0.15
# A press shorter than the debounce time (20 ms, 2 samples) gives nothing.
# Two samples see a burst of presses only if the burst outlasts the
# shortest sample gap, TICK_S minus the tick's jitter: a burst that ran
# longer than that on the host is tried again, so the check never depends
# on luck.
GLITCH_S = 0.004
# Chatter: 3 presses of 1 ms every 2 ms, 5 ms from the first press to the
# end of the last, so the whole burst fits between two samples.
CHATTER_N, CHATTER_PERIOD_S = 3, 0.002
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
# Longer than any tick gap that misses no tick (two periods).
LEAD_S = 3 * TICK_S
# How long a run waits for the event that ends it. A stall that delays
# the event this long also shows as missed ticks, so the run is retried.
EVENT_WAIT_S = 2.0


def judged(missed, within_bound):
    """True when a timed run counts: no tick was missed during it and its own bound held.

    Every timing check goes through here. The window runs from the "ticks"
    call that opens it to the one that closes it. The guest keeps the
    tick on a fixed grid, and a callback late by a whole period or more
    adds to tick_missed() (src/drv/tick.c).

    Before the first callback: "ticks" resets the gap and the spread at
    the next callback, so the gap that ends there is lost. A stall of a
    period or more there makes that callback late by a period or more,
    which `missed` counts. A shorter stall makes it late by less than a
    period, so it runs within two periods of the call. The run starts
    LEAD_S, three periods, after the call, so that stall ends before the
    run. It also becomes the spread's reference, so the spread grows too.

    Between callbacks: a stall shorter than a period shows in the gap
    and the spread. A longer one also counts in `missed`.

    After the last callback: a stall still running when the closing
    "ticks" comes leaves its timer interrupt pending. The guest takes it
    before it reads and parses the command line, so the late callback
    updates `missed` and the gap before the reply. A level change made
    during that stall is still held when sampling resumes, and a turn
    ends at rest for SETTLE_S, so no state is lost past the window.

    So no stall hides from both `missed` and the gap. A gap-only bound
    still needs `missed == 0`: a miss at the first callback clears no
    count, but its lost gap is not measured.
    """
    return missed == 0 and within_bound


def timing_ok(short_s, missed, jitter_s, state_s=LIMIT_STATE_S):
    """The bound for the limit runs. short_s is slip()'s short value: each state's lower bound."""
    return judged(missed, jitter_s < 2 * (state_s - short_s) - 2 * TICK_S)


def markers(vm, name):
    return [m for m in vm.markers() if m.get("stage") == "input" and m.get("ev") == name]


def gpio_events(vm):
    return [(m["type"], m["code"], int(m["value"]))
            for m in markers(vm, "event") if m["src"] == "gpio"]


def total(vm):
    reply = vm.script("totals")
    return int(re.search(r"dial0=(-?\d+)", " ".join(reply.lines)).group(1))


def ticks(vm):
    """(missed ticks since boot, longest tick gap in s, lateness spread in s).

    The gap and the spread cover the tick callbacks since the last call.
    The missed count never resets: subtract the count read before a run.
    """
    m = re.search(r"missed=(\d+) maxgap=(\d+) jitter=(\d+)", " ".join(vm.script("ticks").lines))
    return int(m.group(1)), int(m.group(2)) / 1e6, int(m.group(3)) / 1e6


def timed(vm, run):
    """Calls run() and returns (ticks missed, longest tick gap, jitter, its result) for that call.

    The rule for every timed check: the window opens before the first pin
    change of the run and closes only after the guest processed the last
    effect of the run. So run() returns only after it saw the last event
    the run can cause, see until(), or, for a run that must cause no
    event, after settle().

    "ticks" resets the gap and the spread at the next callback, so the gap
    that ends there is not measured. A gap of two periods or more counts a
    missed tick, so waiting LEAD_S first makes sure the run starts after
    that callback, or the run sees a missed tick.
    """
    m0, _, _ = ticks(vm)
    time.sleep(LEAD_S)
    out = run()
    missed, gap, jitter = ticks(vm)
    return missed - m0, gap, jitter, out


def start(vm, status="active"):
    vm.wait_for(re.compile(r"@RC stage=input ev=ready\n"), timeout=TIMEOUT)
    vm.wait_for("ready\n", timeout=TIMEOUT, port=1)
    gpio = markers(vm, "gpio")
    assert len(gpio) == 1 and gpio[0]["status"] == status, vm.serial
    return PiGpio(vm, vm_board(vm), names=PINS)


def vm_board(vm):
    return vm.qmp.machine


def settle():
    """Holds the lines still for SETTLE_S: a late sample, the debounce and one more tick fit in it."""
    time.sleep(SETTLE_S)


def until(vm, n, done):
    """Waits until done(events since index n) holds, then settles. Returns the events since n.

    Gives up after EVENT_WAIT_S, so a lost event fails the check that
    follows, not the wait.
    """
    deadline = time.monotonic() + EVENT_WAIT_S
    while not done(gpio_events(vm)[n:]) and time.monotonic() < deadline:
        time.sleep(0.02)
    settle()  # nothing more may come
    return gpio_events(vm)[n:]


def released(name):
    """The release of key `name`: the last event a tap of it causes."""
    return lambda ev: ("key", name, 0) in ev


def turned(detents):
    """The last detent of a turn: only it brings the count of its sign to abs(detents)."""
    step = ("dial", "0", 1 if detents > 0 else -1)
    return lambda ev: ev.count(step) >= abs(detents)


def short_pulses(vm, io, schedule, excused):
    """Plays presses whose whole span fits between two samples, so one sample sees them at most.

    The span runs from the start of the first press's write to the return
    of the write that ended the last press: its upper bound, see
    pressed_span(). Two samples are at least TICK_S minus the tick's
    jitter apart while no tick is missed. A run that breaks this, by the
    ticks missed and the jitter measured during that run, is played
    again, then skipped. The events such a run gave go into the set
    `excused`, by index.
    """
    for _ in range(JITTER_TRIES):
        n = len(gpio_events(vm))

        def run():
            io.play(schedule)
            settle()

        missed, _, jitter, _ = timed(vm, run)
        span = pressed_span(schedule, io.log, 0)
        if judged(missed, one_sample_at_most(span, missed, jitter, TICK_S)):
            return
        excused.update(range(n, len(gpio_events(vm))))
    hosttiming.skip(f"pressed for up to {span * 1000:.1f} ms, "
                f"{missed} ticks missed, tick jitter {jitter * 1000:.1f} ms")


def held_tap(vm, io, name, hold_s=HOLD_S):
    """Taps `name` and returns its events. Retried, then skipped, while a tick gap could hide it.

    Two samples in a row see a press when no gap reaches half the hold.
    """
    for _ in range(JITTER_TRIES):
        n = len(gpio_events(vm))

        def run():
            io.tap(name, hold_s)
            return until(vm, n, released(name))

        missed, gap, _, got = timed(vm, run)
        if judged(missed, gap < hold_s / 2):
            return got
    hosttiming.skip(f"a tick gap of {gap * 1000:.1f} ms, {missed} ticks missed")


def turn_ok(vm, io, detents, state_s, lengths=(1, 1, 1, 1), bounce=None, excused=None):
    """Turns the dial and returns (total change, events). Retried, then skipped, while a gap could hide a state.

    No state is hidden while every tick gap is shorter than the shortest
    state, less what the host cut from it and the bounce time. The cut
    comes from slip(): the lower bound of each state, from the return of
    one write to the start of the next. A 2 ms
    state in `lengths` is ignored: its neighbours are seen, so a gap hides
    that one state at most and the decoder infers it.

    A run that is tried again may have lost or added steps. Its change of
    the total goes into the list `excused`, so a check of the whole total
    can leave it out. It does not shift the next run: every turn ends at
    the rest state, held for SETTLE_S, and the decoder clears its partial
    count at the rest state (quad_rest_clears_any_history in
    tests/host/test_input_quad.c).
    """
    long_s = state_s * min(f for f in lengths if f * state_s > 2 * HIDDEN_S)
    bounce_s = bounce[1] * bounce[2] if bounce else 0.0
    for _ in range(JITTER_TRIES):
        n, t0 = len(gpio_events(vm)), total(vm)
        missed, gap, _, (got, ev) = timed(vm, lambda: turn_run(vm, io, n, t0, detents, state_s,
                                                                lengths, bounce))
        _, short = slip(io.log)
        if judged(missed, gap < long_s - short - bounce_s):
            return got, ev
        if excused is not None:
            excused.append(got)
    hosttiming.skip(f"a tick gap of {gap * 1000:.1f} ms, a state cut by "
                f"{short * 1000:.1f} ms, {missed} ticks missed")


def turn_run(vm, io, n, t0, detents, state_s, lengths=(1, 1, 1, 1), bounce=None):
    """One turn for timed(): turns, waits for its last detent, settles. Returns (change of the total, events)."""
    io.turn(DIAL_A, DIAL_B, detents, state_s, lengths, bounce)
    ev = until(vm, n, turned(detents))
    return total(vm) - t0, ev


def no_exception(vm):
    assert not [m for m in vm.markers() if m.get("ev") == "exception"], vm.serial


def test_gpio_buttons(board, example_image):
    """Each button gives one press and one release; a burst that one sample at most can see gives nothing.

    Only runs where no two samples could fall inside the burst count:
    two pressed samples in a row are a valid press for the debouncer.
    """
    with boot(example_image("input"), board, serial2=True, qtest=True,
              gpio_high=ALL_PINS) as vm:
        io = start(vm)
        for name in PINS:
            assert held_tap(vm, io, name) == [("key", name, 1), ("key", name, 0)], vm.serial
        sel, excused, n = PINS["select"], set(), len(gpio_events(vm))
        for _ in range(5):
            short_pulses(vm, io, [(0, sel, 0), (GLITCH_S, sel, 1)], excused)
            time.sleep(3 * TICK_S)
        # Chatter, 1 ms pressed each time, ends released.
        short_pulses(vm, io, bounce_schedule(PINS["back"], True, CHATTER_N, CHATTER_PERIOD_S,
                                             False), excused)
        time.sleep(SETTLE_S)
        got = [e for i, e in enumerate(gpio_events(vm)) if i >= n and i not in excused]
        assert got == [], vm.serial
        no_exception(vm)


def test_gpio_repeat(board, example_image):
    """A held key repeats after 300 ms, then every 100 ms (the repeat variant).

    The guest times the hold from the debounced press to the debounced
    release. Each lands up to two tick gaps after its edge, and a repeat
    waits for a callback, so the guest's hold is the host's within three
    gaps. A run where that can leave the 800 to 1100 ms window of 6 to 8
    repeats is tried again, then skipped.
    """
    img = example_image("input", "repeat", "-DINPUT_DEMO_REPEAT=1")
    with boot(img, board, serial2=True, qtest=True, gpio_high=ALL_PINS) as vm:
        io = start(vm)
        for _ in range(JITTER_TRIES):
            n = len(gpio_events(vm))

            def run():
                t0 = time.perf_counter()
                io.tap("up", 1.0)
                held = time.perf_counter() - t0
                return held, until(vm, n, released("up"))

            missed, gap, _, (held, ev) = timed(vm, run)
            if judged(missed, held - 3 * gap >= 0.8 and held + 3 * gap < 1.1):
                break
        else:
            hosttiming.skip(f"held {held * 1000:.1f} ms, a tick gap of "
                        f"{gap * 1000:.1f} ms, {missed} ticks missed")
        assert ev[0] == ("key", "up", 1) and ev[-1] == ("key", "up", 0), vm.serial
        reps = [e for e in ev if e == ("key", "up", 2)]
        # 1000 ms held: (1000 - 300) / 100 = 7 repeats, one less or more for tick and host timing.
        assert 6 <= len(reps) <= 8 and len(ev) == len(reps) + 2, (held, gap, vm.serial)


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
        t0, excused = total(vm), []
        for detents in (3, -2):
            got, ev = turn_ok(vm, io, detents, SLOW_STATE_S, excused=excused)
            sign = 1 if detents > 0 else -1
            assert (got, ev) == (detents, [("dial", "0", sign)] * abs(detents)), vm.serial
        assert held_tap(vm, io, "dial_push") == [("key", "dial_push", 1),
                                                  ("key", "dial_push", 0)], vm.serial
        # No step outside the judged runs and the excused ones.
        assert total(vm) - t0 == 1 + sum(excused), (excused, vm.serial)


def test_gpio_dial_bounce(board, example_image):
    """Contact bounce on channel A, 3 flips of 1 ms per edge, adds nothing."""
    with boot(example_image("input"), board, serial2=True, qtest=True,
              gpio_high=ALL_PINS) as vm:
        io = start(vm)
        t0, excused = total(vm), []
        for detents in (4, -4):
            got, ev = turn_ok(vm, io, detents, SLOW_STATE_S, bounce=(DIAL_A, 3, 0.001),
                              excused=excused)
            sign = 1 if detents > 0 else -1
            assert (got, ev) == (detents, [("dial", "0", sign)] * 4), vm.serial
        # A run tried again for bad timing may have counted wrong: leave it out.
        # A step outside every run still fails here.
        assert total(vm) - t0 == sum(excused), (excused, vm.serial)


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
            got, _ = turn_ok(vm, io, detents, SLOW_STATE_S, lengths)
            assert got == detents, (detents, lengths, vm.serial)
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
                    n, t0 = len(gpio_events(vm)), total(vm)
                    missed, _, jitter, (got, _) = timed(
                        vm, lambda: turn_run(vm, io, n, t0, detents, LIMIT_STATE_S, lengths))
                    _, short = slip(io.log)
                    if timing_ok(short, missed, jitter):
                        break
                else:
                    hosttiming.skip(f"a state cut by {short * 1000:.1f} ms, "
                                f"{missed} ticks missed, tick jitter {jitter * 1000:.1f} ms")
                assert got == detents, (lengths, short, missed, jitter, vm.serial)
        # Two transitions out and back: a correct phase gives no event.
        wiggle = [(0, DIAL_A, 0), (SLOW_STATE_S, DIAL_B, 0), (2 * SLOW_STATE_S, DIAL_B, 1),
                  (3 * SLOW_STATE_S, DIAL_A, 1), (4 * SLOW_STATE_S, None, None)]
        for _ in range(JITTER_TRIES):
            n = len(gpio_events(vm))

            def run():
                io.play(wiggle)
                settle()

            missed, gap, _, _ = timed(vm, run)
            _, short = slip(io.log)
            if judged(missed, gap < SLOW_STATE_S - short):
                break
        else:
            hosttiming.skip(f"a tick gap of {gap * 1000:.1f} ms, {missed} ticks missed")
        assert len(gpio_events(vm)) == n, vm.serial
        no_exception(vm)
