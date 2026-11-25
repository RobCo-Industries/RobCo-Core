"""The Pi GPIO level helpers: the sequences and the qtest writes. No QEMU needed."""

import pytest

from harness import pigpio
from harness.pigpio import CW, PiGpio, bounce_schedule, quad_schedule, quad_states


def decode(levels):
    """Reference decoder: +1 per clockwise transition, -1 per counter-clockwise one."""
    pos = [CW.index(lv) for lv in levels]
    total = 0
    for a, b in zip(pos, pos[1:]):
        d = (b - a) % 4
        assert d in (0, 1, 3), (a, b)
        total += {0: 0, 1: 1, 3: -1}[d]
    return total


def replay(schedule, a=23, b=24):
    """Levels of (A, B) after each change, starting at rest."""
    lv = {a: 1, b: 1}
    out = [(1, 1)]
    for _, pin, level in schedule:
        if pin is not None:
            lv[pin] = level
            out.append((lv[a], lv[b]))
    return out


def test_quad_states_both_ways():
    assert quad_states(1) == [1, 2, 3, 0]
    assert quad_states(-1) == [3, 2, 1, 0]
    assert quad_states(3)[-1] == 0 and len(quad_states(-3)) == 12
    assert quad_states(0) == []


def test_clockwise_matches_the_library_decoder():
    # src/lib/input_quad.c: 11 -> 10 is +1 with state (A << 1) | B.
    assert CW[:2] == ((1, 1), (1, 0))


@pytest.mark.parametrize("detents", [1, 5, -1, -7])
def test_schedule_is_gray_and_lands_at_rest(detents):
    s = quad_schedule(23, 24, detents, 0.01)
    lv = replay(s)
    assert lv[-1] == (1, 1)
    assert decode(lv) == 4 * detents
    # One pin per state change, at multiples of the state time.
    times = [t for t, pin, _ in s if pin is not None]
    assert times == pytest.approx([0.01 * i for i in range(4 * abs(detents))])
    assert s[-1] == (pytest.approx(0.04 * abs(detents)), None, None)


def test_schedule_unequal_lengths():
    s = quad_schedule(23, 24, 2, 0.01, lengths=(3, 1, 1, 1))
    times = [t for t, pin, _ in s if pin is not None]
    # Entering rest (position 0) lasts 3 states.
    assert times == pytest.approx([0, 0.01, 0.02, 0.03, 0.06, 0.07, 0.08, 0.09])
    assert s[-1][0] == pytest.approx(0.12)


def test_schedule_bounce_on_one_channel():
    s = quad_schedule(23, 24, 1, 0.01, bounce=(23, 3, 0.001))
    on_a = [(t, lv) for t, pin, lv in s if pin == 23]
    on_b = [(t, lv) for t, pin, lv in s if pin == 24]
    assert len(on_b) == 2                      # B is clean
    assert len(on_a) == 2 * (2 * 3 + 1)        # each A change: 3 bounces then the level
    assert decode(replay(s)) == 4              # bounce on one line nets nothing
    assert replay(s)[-1] == (1, 1)


def test_bounce_schedule_settles():
    s = bounce_schedule(5, True, 4, 0.002, settle=False)
    assert [lv for _, _, lv in s] == [0, 1] * 4 + [1]
    assert bounce_schedule(5, False, 1, 0.002, settle=True)[-1][2] == 1


def test_level_writes():
    assert pigpio.level_writes("raspi0", 17, 1) == (0x2020001C, 1 << 17)
    assert pigpio.level_writes("raspi2b", 17, 0) == (0x3F200028, 1 << 17)
    assert pigpio.level_writes("raspi3b", 47, 1) == (0x3F200020, 1 << 15)


class FakeQemu:
    def __init__(self):
        self.lev = [0, 0]
        self.writes = []

    def writel(self, addr, val):
        self.writes.append((addr, val))
        off = addr - 0x20200000
        bank = (off - (0x1C if off < 0x28 else 0x28)) // 4
        if off < 0x28:
            self.lev[bank] |= val
        else:
            self.lev[bank] &= ~val

    def readl(self, addr):
        return self.lev[(addr - 0x20200034) // 4]


def test_pigpio_names_and_levels():
    q = FakeQemu()
    io = PiGpio(q, "raspi0", names={"up": 5})
    io.release("up")
    assert io.get("up") == 1 and io.get(5) == 1
    io.press("up")
    assert io.get(5) == 0
    io.set(40, 1)
    assert io.get(40) == 1
    late = io.turn(23, 24, 1, 0.001)
    assert late < 0.05
    assert (io.get(23), io.get(24)) == (1, 1)
    assert len(q.writes) == 3 + 4


def test_slip_measures_late_writes_and_shortened_states():
    # Planned at 0, 10, 20 ms. The second write ran 4 ms late, the third on time.
    log = [(0.0, 0.0), (0.010, 0.014), (0.020, 0.020)]
    late, short = pigpio.slip(log)
    assert late == pytest.approx(0.004)
    assert short == pytest.approx(0.004)     # the state from 14 to 20 ms lasted 6 ms
    # A late last write makes the state longer, not shorter.
    assert pigpio.slip([(0.0, 0.0), (0.010, 0.010), (0.020, 0.025)])[1] == 0.0
    # A late first write shortens the state after it.
    assert pigpio.slip([(0.0, 0.005), (0.010, 0.010)])[1] == pytest.approx(0.005)
    # Two writes planned at the same time (a bounce) never count as shortened.
    assert pigpio.slip([(0.0, 0.0), (0.0, 0.001)]) == (pytest.approx(0.001), 0.0)
    assert pigpio.slip([]) == (0.0, 0.0)


def test_play_keeps_a_log():
    io = PiGpio(FakeQemu(), "raspi0")
    io.play([(0.0, 5, 1), (0.002, 5, 0), (0.004, None, None)])
    assert [t for t, _ in io.log] == [0.0, 0.002, 0.004]
    assert all(real >= t for t, real in io.log)


def test_play_lateness_counts_a_slow_write(monkeypatch):
    # A fake clock that only moves when the write blocks for 7 ms.
    clock = [0.0]

    class SlowQemu(FakeQemu):
        def writel(self, addr, val):
            clock[0] += 0.007
            super().writel(addr, val)

    monkeypatch.setattr(pigpio.time, "perf_counter", lambda: clock[0])
    monkeypatch.setattr(pigpio.time, "sleep", lambda s: clock.__setitem__(0, clock[0] + max(s, 1e-6)))
    io = PiGpio(SlowQemu(), "raspi0")
    late = io.play([(0.0, 5, 1), (0.010, 5, 0)])
    assert late >= 0.007
    assert late == pytest.approx(pigpio.slip(io.log)[0])


def test_pressed_span_covers_the_burst():
    glitch = [(0.0, 5, 0), (0.004, 5, 1)]
    # The release ran 1 ms late: the press may have lasted 5 ms.
    assert pigpio.pressed_span(glitch, [(0.0, 0.0001), (0.004, 0.005)], 0) == pytest.approx(0.005)
    chatter = bounce_schedule(5, True, 3, 0.002, False)
    on_time = [(t, t) for t, _, _ in chatter]
    assert pigpio.pressed_span(chatter, on_time, 0) == pytest.approx(0.005)
    assert pigpio.pressed_span(bounce_schedule(5, True, 2, 0.002, True), on_time, 0) == float("inf")
    assert pigpio.pressed_span([(0.0, None, None)], [(0.0, 0.0)], 0) == 0.0


def test_old_chatter_check_let_two_samples_see_a_press():
    """The old chatter, 2 ms pressed every 4 ms, with samples at 1 and 9 ms."""
    old = bounce_schedule(5, True, 5, 0.004, False)
    pressed = [(t, t + 0.002) for t, _, lv in old if lv == 0]
    assert any(a <= 0.001 < b for a, b in pressed) and any(a <= 0.009 < b for a, b in pressed)
    jitter, late = 0.002, 0.0                   # samples 8 ms apart
    # The old check looked at one press only and let this run count.
    assert 0.002 + late < 0.010 - jitter
    # The burst check sees 18 ms between the first press and the last release.
    span = pigpio.pressed_span(old, [(t, t) for t, _, _ in old], 0)
    assert span == pytest.approx(0.018)
    assert not pigpio.one_sample_at_most(span, 0, jitter, 0.010)
    # The new chatter fits between those samples whatever their phase.
    new = bounce_schedule(5, True, 3, 0.002, False)
    assert pigpio.one_sample_at_most(pigpio.pressed_span(new, [(t, t) for t, _, _ in new], 0),
                                     0, jitter, 0.010)


def test_one_sample_at_most():
    assert pigpio.one_sample_at_most(0.004, 0, 0.003, 0.010)
    assert not pigpio.one_sample_at_most(0.004, 0, 0.006, 0.010)
    assert not pigpio.one_sample_at_most(0.004, 1, 0.0, 0.010)
