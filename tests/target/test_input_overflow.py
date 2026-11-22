"""Boot examples/input on every board and overflow the queue under each policy."""

import pytest

from harness.boot import boot

from test_input import events, markers, no_exception, start, wait_count

QUEUE = 32
CHARS = "0123456789abcdefghijklmnopqrstuvwxyzABCD"     # 40 events in one line
BURST = 40


def burst_value(i):
    """The value examples/input gives event i of "burst"."""
    return i if i % 3 else -i


@pytest.mark.parametrize("policy", ["newest", "oldest"])
def test_input_overflow(board, example_image, policy):
    """More than 32 events before the image takes any: the policy picks which 32 stay.

    The dial totals count every step, the dropped ones too.
    """
    oldest = policy == "oldest"
    if oldest:
        image = example_image("input", "drop_oldest", "-DINPUT_DEMO_DROP_OLDEST=1")
    else:
        image = example_image("input")
    keep = slice(-QUEUE, None) if oldest else slice(0, QUEUE)
    dropped = 0

    with boot(image, board, serial2=True) as vm:
        start(vm)
        expected = []

        # Phase 1: one built-in line pushes 40 CHAR events in one go.
        r = vm.script("text " + CHARS)
        assert r == (("ok dropped" if oldest else "err full"), []), vm.serial2
        expected += [("char", str(ord(c)), 0) for c in CHARS[keep]]
        dropped += len(CHARS) - QUEUE
        assert wait_count(vm, lambda: events(vm), len(expected)) == expected, vm.serial

        # Phase 2: 40 DIAL events on dials 0 and 1 in one go.
        r = vm.script(f"burst {BURST}")
        over = BURST - QUEUE
        want = f"pushed={BURST} full={0 if oldest else over} oldest={over if oldest else 0}"
        assert r == ("ok", [want]), vm.serial2
        steps = list(range(1, BURST + 1))
        expected += [("dial", str(i % 2), burst_value(i)) for i in steps[keep]]
        dropped += over
        assert wait_count(vm, lambda: events(vm), len(expected)) == expected, vm.serial

        # Phase 3: a built-in dial line on an empty queue, then the totals.
        assert vm.script("dial -7").status == "ok", vm.serial2
        expected += [("dial", "0", -7)]
        assert wait_count(vm, lambda: events(vm), len(expected)) == expected, vm.serial
        dial0 = sum(burst_value(i) for i in steps if i % 2 == 0) - 7
        dial1 = sum(burst_value(i) for i in steps if i % 2 == 1)
        r = vm.script("totals")
        assert r == ("ok", [f"dial0={dial0} dial1={dial1} dropped={dropped}"]), vm.serial2

        # Nothing came after the last expected event.
        assert events(vm) == expected, vm.serial
        assert len(markers(vm, "event")) == len(expected), vm.serial
        no_exception(vm)
