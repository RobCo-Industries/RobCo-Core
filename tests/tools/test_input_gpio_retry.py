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
