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
