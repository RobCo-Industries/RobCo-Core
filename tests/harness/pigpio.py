"""Drive Raspberry Pi GPIO input levels from outside a running guest.

QEMU's bcm2835_gpio has no input lines, but a qtest write to GPSET or
GPCLR sets or clears the GPLEV bit of any pin, inputs included (canary
V18). Pulls are not modelled, so an undriven pin reads 0: drive every
active-low line high before the guest starts, with boot(..., qtest=True,
gpio_high=[...]).

    with boot(img, board, qtest=True, gpio_high=PINS.values()) as vm:
        io = PiGpio(vm, board, names=PINS)
        io.tap("select", 0.1)
        io.turn(23, 24, detents=3, state_s=0.02)

The sequence functions are pure and need no QEMU. Timing follows the
host clock, which QEMU's system timer tracks (V18); one level write takes
about 0.1 ms. The host scheduler adds jitter of about a millisecond.
"""

import time

PERIPH_BASE = {"raspi0": 0x20000000, "raspi1ap": 0x20000000,
               "raspi2b": 0x3F000000, "raspi3b": 0x3F000000}
GPIO_OFFSET = 0x200000
GPSET0, GPCLR0, GPLEV0 = 0x1C, 0x28, 0x34

# Clockwise Gray sequence of the levels (A, B), starting at rest with both high.
CW = ((1, 1), (1, 0), (0, 0), (0, 1))


def gpio_base(board):
    return PERIPH_BASE[board] + GPIO_OFFSET


def level_writes(board, pin, level):
    """(address, value) of the qtest write that drives `pin` to `level`."""
    reg = GPSET0 if level else GPCLR0
    return gpio_base(board) + reg + 4 * (pin // 32), 1 << (pin % 32)


def quad_states(detents, start=0):
    """Gray positions (index into CW) for `detents` detents, sign is direction.

    Four transitions per detent. The first position is the one after
    `start`; the last one is a rest position when `start` is.
    """
    step = 1 if detents > 0 else -1
    pos, out = start, []
    for _ in range(4 * abs(detents)):
        pos = (pos + step) % 4
        out.append(pos)
    return out


def quad_schedule(pin_a, pin_b, detents, state_s, lengths=(1, 1, 1, 1),
                  bounce=None, start=0):
    """The level changes of a dial turn as a list of (t, pin, level).

    Each position lasts state_s times its factor in `lengths`, indexed by
    the Gray position entered (0 is rest, both high). Unequal factors model
    an uneven hand. bounce=(pin, count, period_s) makes that channel chatter
    `count` extra times, each period_s long, every time it changes. The
    bounce ends on the final level, inside the state's time. t starts at 0.
    """
    out, t, pos = [], 0.0, start
    for nxt in quad_states(detents, start):
        old, new = CW[pos], CW[nxt]
        for i, pin in enumerate((pin_a, pin_b)):
            if old[i] == new[i]:
                continue
            if bounce and bounce[0] == pin:
                count, period = bounce[1], bounce[2]
                for k in range(count):
                    out.append((t + k * period, pin, new[i]))
                    out.append((t + (k + 0.5) * period, pin, old[i]))
                out.append((t + count * period, pin, new[i]))
            else:
                out.append((t, pin, new[i]))
        t += state_s * lengths[nxt]
        pos = nxt
    out.append((t, None, None))  # the end of the last state
    return out


def bounce_schedule(pin, active_low, count, period_s, settle):
    """A button that chatters `count` times then rests pressed (settle=True) or released."""
    press = 0 if active_low else 1
    out, t = [], 0.0
    for k in range(count):
        out.append((t, pin, press))
        out.append((t + period_s / 2, pin, 1 - press))
        t += period_s
    out.append((t, pin, press if settle else 1 - press))
    return out


class PiGpio:
    """Levels of the Pi's own GPIO pins through qtest. Needs boot(..., qtest=True).

    `target` is a Vm from boot() or a Qemu. `names` maps button names to
    pins for press() and tap(). Buttons are active low unless active_low
    is False.
    """

    def __init__(self, target, board, names=None, active_low=True):
        self.q = getattr(target, "qmp", target)
        self.board = board
        self.names = dict(names or {})
        self.active_low = active_low

    def _pin(self, pin):
        return self.names[pin] if isinstance(pin, str) else pin

    def set(self, pin, level):
        addr, val = level_writes(self.board, self._pin(pin), level)
        self.q.writel(addr, val)

    def get(self, pin):
        """GPLEV bit of `pin`: an outside level, or what the guest drives on an output."""
        pin = self._pin(pin)
        return self.q.readl(gpio_base(self.board) + GPLEV0 + 4 * (pin // 32)) >> (pin % 32) & 1

    def press(self, pin):
        self.set(pin, 0 if self.active_low else 1)

    def release(self, pin):
        self.set(pin, 1 if self.active_low else 0)

    def tap(self, pin, hold_s):
        """Press, hold for hold_s seconds, release."""
        self.press(pin)
        time.sleep(hold_s)
        self.release(pin)

    def play(self, schedule):
        """Applies (t, pin, level) changes at their times. Returns the seconds it ran late at most."""
        t0, late = time.perf_counter(), 0.0
        for t, pin, level in schedule:
            while True:
                now = time.perf_counter() - t0
                if now >= t:
                    break
                if t - now > 0.003:
                    time.sleep(t - now - 0.002)  # then spin: sleep overshoots
            late = max(late, now - t)
            if pin is not None:
                self.set(pin, level)
        return late

    def bounce(self, pin, count, period_s, settle=False):
        return self.play(bounce_schedule(self._pin(pin), self.active_low, count, period_s, settle))

    def turn(self, pin_a, pin_b, detents, state_s, lengths=(1, 1, 1, 1), bounce=None):
        """Turns a dial at rest (both lines high) by `detents`, positive is clockwise.

        Returns the most the schedule ran late, in seconds.
        """
        return self.play(quad_schedule(self._pin(pin_a), self._pin(pin_b), detents, state_s,
                                       lengths, bounce))
