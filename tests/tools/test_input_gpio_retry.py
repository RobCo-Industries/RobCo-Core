"""short_pulses() in the target GPIO test decides on the run's own tick counts. No QEMU needed."""

import importlib.util
import pathlib

import pytest

SRC = pathlib.Path(__file__).resolve().parents[1] / "target" / "test_input_gpio.py"
spec = importlib.util.spec_from_file_location("target_input_gpio", SRC)
gpio_test = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gpio_test)


class Reply:
    def __init__(self, line):
        self.lines = [line]


class FakeVm:
    """Answers "ticks" with a missed count since boot that grows by `per_run` during each play."""

    def __init__(self, missed_at_start, per_run):
        self.missed = missed_at_start
        self.per_run = per_run

    def script(self, cmd):
        assert cmd == "ticks"
        return Reply(f"count=1 missed={self.missed} maxgap=10000 jitter=1000")

    def markers(self):
        return []


class FakeIo:
    def __init__(self, vm):
        self.vm = vm

    def play(self, schedule):
        self.log = [(t, t, t + 0.0001) for t, _, _ in schedule]
        self.vm.missed += self.vm.per_run


GLITCH = [(0.0, 26, 0), (0.004, 26, 1)]


@pytest.fixture(autouse=True)
def no_sleep(monkeypatch):
    monkeypatch.setattr(gpio_test.time, "sleep", lambda s: None)
    monkeypatch.setattr(gpio_test, "EVENT_WAIT_S", 0.0, raising=False)  # a lost event ends the wait at once


def test_a_tick_missed_before_the_pulse_does_not_reject_it():
    vm = FakeVm(missed_at_start=3, per_run=0)
    excused = set()
    gpio_test.short_pulses(vm, FakeIo(vm), GLITCH, excused)
    assert excused == set()


def test_a_tick_missed_during_the_pulse_rejects_it():
    vm = FakeVm(missed_at_start=3, per_run=1)
    with pytest.raises(pytest.skip.Exception, match="1 ticks missed"):
        gpio_test.short_pulses(vm, FakeIo(vm), GLITCH, set())


class DialVm:
    """A guest whose dial counts what each turn is told to count, with a tick gap per turn.

    A turn may stall the guest after its last level change: the guest then
    reports its last step only on the next look at its output, and the
    stall counts as missed ticks from then on.
    """

    def __init__(self, runs):
        # (steps counted, longest tick gap in s[, ticks missed[, ticks missed before the last step]])
        self.runs = list(runs)
        self.dial = 0
        self.gap = 0.0
        self.missed = 0
        self.events = []
        self.late = None
        self.serial = ""

    def report_late(self):
        if self.late:
            event, missed = self.late
            self.late = None
            self.missed += missed
            self.dial += int(event["value"])
            self.events.append(event)

    def script(self, cmd):
        if cmd == "totals":
            self.report_late()
            return Reply(f"dial0={self.dial} dial1=0 dropped=0")
        assert cmd == "ticks"
        return Reply(f"count=1 missed={self.missed} maxgap={int(self.gap * 1e6)} jitter=1000")

    def markers(self):
        self.report_late()
        return self.events

    def turn(self):
        steps, self.gap, *missed = self.runs.pop(0)
        self.missed += missed[0] if missed else 0
        sign = "1" if steps > 0 else "-1"
        event = {"stage": "input", "ev": "event", "src": "gpio", "type": "dial",
                 "code": "0", "value": sign}
        shown = abs(steps)
        if len(missed) > 1:
            shown -= 1
            self.late = (event, missed[1])
        self.dial += shown * int(sign)
        self.events += [event] * shown


class DialIo:
    def __init__(self, vm):
        self.vm = vm

    def turn(self, pin_a, pin_b, detents, state_s, lengths=(1, 1, 1, 1), bounce=None):
        self.log = [(0.0, 0.0, 0.0001)]
        self.vm.turn()


def run_dial_bounce(monkeypatch, runs):
    vm = DialVm(runs)

    class Boot:
        def __enter__(self):
            return vm

        def __exit__(self, *exc):
            return False

    monkeypatch.setattr(gpio_test, "boot", lambda *a, **k: Boot())
    monkeypatch.setattr(gpio_test, "start", lambda v: DialIo(v))
    gpio_test.test_gpio_dial_bounce("raspi2b", lambda *a: "image")
    return vm


GOOD, LATE = 0.010, 0.040   # a tick gap that hides no 30 ms state, and one that may


def test_a_retried_turn_that_lost_a_step_does_not_fail_the_total(monkeypatch):
    # A late host made the first +4 count 3. It is tried again and counts 4.
    vm = run_dial_bounce(monkeypatch, [(3, LATE), (4, GOOD), (-4, GOOD)])
    assert vm.runs == [] and vm.dial == 3


def test_a_step_outside_every_turn_still_fails(monkeypatch):
    real_total, reads = gpio_test.total, []

    def total_with_a_stray_step(v):
        reads.append(1)
        if len(reads) == 6:         # the final read: a step came after the last turn
            v.dial += 1
        return real_total(v)

    monkeypatch.setattr(gpio_test, "total", total_with_a_stray_step)
    with pytest.raises(AssertionError):
        run_dial_bounce(monkeypatch, [(4, GOOD), (-4, GOOD)])


def test_a_judged_turn_that_lost_a_step_still_fails(monkeypatch):
    with pytest.raises(AssertionError):
        run_dial_bounce(monkeypatch, [(3, GOOD), (-4, GOOD)])


def test_a_turn_with_a_missed_tick_and_a_small_gap_is_tried_again(monkeypatch):
    # A stall before the first callback: the gap looks good, a tick was missed.
    vm = run_dial_bounce(monkeypatch, [(3, GOOD, 1), (4, GOOD), (-4, GOOD)])
    assert vm.runs == [] and vm.dial == 3


def test_a_turn_stalled_before_its_last_step_shows_is_tried_again(monkeypatch):
    # The stall comes after the last level change, before the guest reports the last step.
    vm = run_dial_bounce(monkeypatch, [(4, GOOD, 0, 1), (4, GOOD), (-4, GOOD)])
    assert vm.runs == [] and vm.dial == 4


def test_a_turn_with_no_missed_tick_and_a_good_gap_is_judged(monkeypatch):
    vm = run_dial_bounce(monkeypatch, [(4, GOOD), (-4, GOOD)])
    assert vm.runs == [] and vm.dial == 0


class TapVm:
    """A guest that gives a press and a release per tap, with a tick gap and missed ticks per tap.

    A tap may stall the guest after the release: the guest then reports the
    release only on the next look at its output, and the stall counts as
    missed ticks from then on.
    """

    def __init__(self, runs, repeats=0):
        self.runs = list(runs)      # (longest tick gap in s, ticks missed[, missed before the release])
        self.repeats = repeats
        self.gap = 0.0
        self.missed = 0
        self.events = []
        self.late = None
        self.serial = ""
        self.taps = 0

    def script(self, cmd):
        assert cmd == "ticks"
        return Reply(f"count=1 missed={self.missed} maxgap={int(self.gap * 1e6)} jitter=1000")

    def markers(self):
        if self.late:
            event, missed = self.late
            self.late = None
            self.missed += missed
            self.events.append(event)
        return self.events

    def tap(self, name):
        self.taps += 1
        self.gap, missed, *late = self.runs.pop(0)
        self.missed += missed
        ev = [{"stage": "input", "ev": "event", "src": "gpio", "type": "key",
               "code": name, "value": v} for v in ["1"] + ["2"] * self.repeats + ["0"]]
        if late:
            self.late = (ev.pop(), late[0])
        self.events += ev


class TapIo:
    def __init__(self, vm):
        self.vm = vm

    def tap(self, name, hold_s):
        self.vm.tap(name)


def test_a_held_tap_with_a_missed_tick_and_a_small_gap_is_tried_again():
    vm = TapVm([(0.010, 1), (0.010, 0)])
    got = gpio_test.held_tap(vm, TapIo(vm), "select")
    assert vm.taps == 2 and got == [("key", "select", 1), ("key", "select", 0)]


def test_a_held_tap_stalled_before_its_release_shows_is_tried_again():
    vm = TapVm([(0.010, 0, 1), (0.010, 0)])
    got = gpio_test.held_tap(vm, TapIo(vm), "select")
    assert vm.taps == 2 and got == [("key", "select", 1), ("key", "select", 0)]


def test_a_held_tap_with_no_missed_tick_and_a_good_gap_is_judged():
    vm = TapVm([(0.010, 0)])
    gpio_test.held_tap(vm, TapIo(vm), "select")
    assert vm.taps == 1


def run_repeat(monkeypatch, runs):
    vm = TapVm(runs, repeats=7)

    class Boot:
        def __enter__(self):
            return vm

        def __exit__(self, *exc):
            return False

    clock = iter(range(1000))
    monkeypatch.setattr(gpio_test, "boot", lambda *a, **k: Boot())
    monkeypatch.setattr(gpio_test, "start", lambda v: TapIo(v))
    # Each tap holds 1 s on the fake clock.
    monkeypatch.setattr(gpio_test.time, "perf_counter", lambda: float(next(clock)))
    gpio_test.test_gpio_repeat("raspi2b", lambda *a: "image")
    return vm


def test_a_repeat_with_a_missed_tick_and_a_small_gap_is_tried_again(monkeypatch):
    vm = run_repeat(monkeypatch, [(0.010, 1), (0.010, 0)])
    assert vm.taps == 2


def test_a_repeat_with_no_missed_tick_and_a_good_gap_is_judged(monkeypatch):
    vm = run_repeat(monkeypatch, [(0.010, 0)])
    assert vm.taps == 1


def test_a_repeat_stalled_before_its_release_shows_is_tried_again(monkeypatch):
    # The press and the first repeat already make two events, so only the release ends the run.
    vm = run_repeat(monkeypatch, [(0.010, 0, 1), (0.010, 0)])
    assert vm.taps == 2
