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
        self.log = [(t, t + 0.0001) for t, _, _ in schedule]
        self.vm.missed += self.vm.per_run


GLITCH = [(0.0, 26, 0), (0.004, 26, 1)]


@pytest.fixture(autouse=True)
def no_sleep(monkeypatch):
    monkeypatch.setattr(gpio_test.time, "sleep", lambda s: None)


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
    """A guest whose dial counts what each turn is told to count, with a tick gap per turn."""

    def __init__(self, runs):
        self.runs = list(runs)      # (steps counted, longest tick gap in s) per turn
        self.dial = 0
        self.gap = 0.0
        self.events = []
        self.serial = ""

    def script(self, cmd):
        if cmd == "totals":
            return Reply(f"dial0={self.dial} dial1=0 dropped=0")
        assert cmd == "ticks"
        return Reply(f"count=1 missed=0 maxgap={int(self.gap * 1e6)} jitter=1000")

    def markers(self):
        return self.events

    def turn(self):
        steps, self.gap = self.runs.pop(0)
        self.dial += steps
        sign = "1" if steps > 0 else "-1"
        self.events += [{"stage": "input", "ev": "event", "src": "gpio", "type": "dial",
                         "code": "0", "value": sign}] * abs(steps)


class DialIo:
    def __init__(self, vm):
        self.vm = vm

    def turn(self, pin_a, pin_b, detents, state_s, lengths=(1, 1, 1, 1), bounce=None):
        self.log = [(0.0, 0.0001)]
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
