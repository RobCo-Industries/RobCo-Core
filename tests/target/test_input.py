"""Boot examples/input on every board: GPIO guard, console keys, script channel, repeat."""

import re
import time

from harness.boot import boot

READY = re.compile(r"@RC stage=input ev=ready\n")
TIMEOUT = 60


def markers(vm, name):
    return [m for m in vm.markers() if m.get("stage") == "input" and m.get("ev") == name]


def events(vm, src=None):
    return [(m["type"], m["code"], int(m["value"]))
            for m in markers(vm, "event") if src is None or m["src"] == src]


def wait_count(vm, fn, count, timeout=TIMEOUT):
    """Waits until fn() returns at least `count` items, then returns them."""
    deadline = time.monotonic() + timeout
    while True:
        items = fn()
        if len(items) >= count:
            return items
        assert time.monotonic() < deadline, f"wanted {count}, got {items}\n{vm.serial}"
        time.sleep(0.05)


def replies(vm):
    return [line for line in vm.serial2.split("\n") if line.startswith(("ok", "err"))]


def start(vm):
    vm.wait_for(READY, timeout=TIMEOUT)
    vm.wait_for("ready\n", timeout=TIMEOUT, port=1)
    init = markers(vm, "init")
    assert len(init) == 1, vm.serial
    assert init[0]["mini"] == "0" and init[0]["gpio"] == "0", vm.serial
    assert init[0]["script"] == "0", vm.serial
    # Before uart_mini_init(): INPUT_ERR_NO_UART, and a getc that returns -1 at once.
    assert (init[0]["early_script"], init[0]["early_getc"]) == ("-7", "-1"), vm.serial


def tap(code):
    return [("key", code, 1), ("key", code, 0)]


def no_exception(vm):
    assert not [m for m in vm.markers() if m.get("ev") == "exception"], vm.serial


def test_input_gpio_guard(board, example_image):
    """QEMU inputs read 0, so the active-low buttons look stuck: GPIO turns off."""
    with boot(example_image("input"), board, serial2=True) as vm:
        start(vm)
        gpio = markers(vm, "gpio")
        assert len(gpio) == 1 and gpio[0]["status"] == "disabled_stuck", vm.serial
        time.sleep(0.5)
        # A console byte proves the loop runs; still no GPIO event.
        vm.send("z")
        wait_count(vm, lambda: events(vm), 1)
        assert events(vm, "gpio") == [], vm.serial
        no_exception(vm)


def test_input_console_keys(board, example_image):
    with boot(example_image("input"), board, serial2=True) as vm:
        start(vm)
        expected = []
        vm.send("\x1b[A\x1b[B\x1b[C\x1b[D\r\n\x7f")
        expected += tap("up") + tap("down") + tap("right") + tap("left")
        expected += tap("select") + tap("back")
        wait_count(vm, lambda: events(vm, "serial"), len(expected))

        # A lone ESC becomes BACK once the guest timeout passes.
        vm.send("\x1b")
        expected += tap("back")
        wait_count(vm, lambda: events(vm, "serial"), len(expected))

        vm.send("\x1bOPhi\t")
        expected += tap("f1") + [("char", str(ord("h")), 0), ("char", str(ord("i")), 0),
                                 ("char", "9", 0)]
        wait_count(vm, lambda: events(vm, "serial"), len(expected))
        time.sleep(0.3)
        assert events(vm, "serial") == expected, vm.serial

        times = [int(m["t_ms"]) for m in markers(vm, "event")]
        assert times == sorted(times), vm.serial
        no_exception(vm)


def test_input_script(board, example_image):
    with boot(example_image("input"), board, serial2=True) as vm:
        start(vm)
        vm.send("key up\nkey select 1\nkey select 0\ndial +3\ndial -2\ntext hey\nchar A\n"
                "key f4\nbogus\nkey nope\ndial 0\n", port=1)
        got = wait_count(vm, lambda: replies(vm), 11)
        assert got == ["ok"] * 8 + ["err unknown command", "err unknown key", "err bad steps"], \
            vm.serial2
        expected = tap("up") + [("key", "select", 1), ("key", "select", 0),
                                ("dial", "0", 3), ("dial", "0", -2)]
        expected += [("char", str(ord(c)), 0) for c in "heyA"] + tap("f4")
        wait_count(vm, lambda: events(vm, "script"), len(expected))
        time.sleep(0.3)
        assert events(vm, "script") == expected, vm.serial
        no_exception(vm)


def test_input_script_wait(board, example_image):
    """`wait` holds the next line for at least its time, in guest milliseconds."""
    with boot(example_image("input"), board, serial2=True) as vm:
        start(vm)
        vm.send("key left\nwait 300\nkey right\n", port=1)
        wait_count(vm, lambda: replies(vm), 3)
        assert replies(vm) == ["ok"] * 3, vm.serial2
        ev = wait_count(vm, lambda: markers(vm, "event"), 4)
        left = [int(m["t_ms"]) for m in ev if m["code"] == "left"]
        right = [int(m["t_ms"]) for m in ev if m["code"] == "right"]
        assert len(left) == 2 and len(right) == 2, vm.serial
        assert right[0] - left[1] >= 300, vm.serial


def test_input_script_flood(board, example_image):
    """More events than the queue holds: drops are counted, nothing breaks."""
    with boot(example_image("input"), board, serial2=True) as vm:
        start(vm)
        lines = 6
        vm.send(("text " + "x" * 100 + "\n") * lines + "stats\n", port=1)
        got = wait_count(vm, lambda: replies(vm), lines + 1)
        assert all(r in ("ok", "err full") for r in got[:lines]), vm.serial2
        m = re.match(r"ok pushed=(\d+) dropped=(\d+)$", got[lines])
        assert m, vm.serial2
        pushed, dropped = int(m.group(1)), int(m.group(2))
        assert dropped > 0, vm.serial2
        assert pushed + dropped == 100 * lines, vm.serial2

        # Console stats agree and the system still answers.
        vm.send("q")
        stats = wait_count(vm, lambda: markers(vm, "stats"), 1)
        assert int(stats[0]["dropped"]) == dropped, vm.serial
        assert int(stats[0]["pushed"]) == pushed + 1, vm.serial
        vm.send("key up\n", port=1)
        wait_count(vm, lambda: replies(vm), lines + 2)
        wait_count(vm, lambda: [e for e in events(vm, "script") if e[0] == "key"], 2)
        no_exception(vm)


def test_input_repeat(board, example_image):
    """A key held through the script channel repeats until released."""
    image = example_image("input", "repeat", "-DINPUT_DEMO_REPEAT=1")
    with boot(image, board, serial2=True) as vm:
        start(vm)
        assert markers(vm, "init")[0]["repeat"] == "0", vm.serial
        vm.send("key down 1\nwait 1000\nkey down 0\n", port=1)
        wait_count(vm, lambda: replies(vm), 3)
        wait_count(vm, lambda: [e for e in events(vm, "script") if e[2] == 0], 1)
        ev = events(vm, "script")
        assert ev[0] == ("key", "down", 1), vm.serial
        assert ev[-1] == ("key", "down", 0), vm.serial
        middle = ev[1:-1]
        assert len(middle) >= 1, vm.serial
        assert all(e == ("key", "down", 2) for e in middle), vm.serial

        # Repeats start no earlier than the delay after the press.
        ms = [(int(m["value"]), int(m["t_ms"])) for m in markers(vm, "event")]
        press = ms[0][1]
        assert all(t - press >= 300 for v, t in ms if v == 2), vm.serial

        # A tap without hold does not repeat.
        vm.send("key up\n", port=1)
        wait_count(vm, lambda: replies(vm), 4)
        time.sleep(0.5)
        assert [e for e in events(vm, "script") if e[1] == "up"] == tap("up"), vm.serial
        no_exception(vm)


def test_input_two_sources_hold(board, example_image):
    """A console tap on a key the script holds does not release it or stop its repeat."""
    image = example_image("input", "repeat", "-DINPUT_DEMO_REPEAT=1")
    with boot(image, board, serial2=True) as vm:
        start(vm)
        vm.send("key down 1\n", port=1)
        wait_count(vm, lambda: replies(vm), 1)
        wait_count(vm, lambda: events(vm, "script"), 1)
        vm.send("\x1b[B")
        wait_count(vm, lambda: events(vm, "serial"), 2)
        ser = [m for m in markers(vm, "event") if m["src"] == "serial"]
        # Each source's events pass through; the key stays down after the tap.
        assert [(m["value"], m["down"]) for m in ser] == [("1", "1"), ("0", "1")], vm.serial
        tap_ms = int(ser[1]["t_ms"])

        # The repeat goes on after the tap, from the source still holding.
        def late_repeats():
            return [m for m in markers(vm, "event")
                    if m["value"] == "2" and int(m["t_ms"]) > tap_ms + 400]
        rep = wait_count(vm, late_repeats, 1)
        assert all(m["src"] == "script" and m["down"] == "1" for m in rep), vm.serial

        vm.send("key down 0\n", port=1)
        wait_count(vm, lambda: [e for e in events(vm, "script") if e[2] == 0], 1)
        rel = [m for m in markers(vm, "event") if m["src"] == "script" and m["value"] == "0"]
        assert rel[0]["down"] == "0", vm.serial
        time.sleep(0.5)
        last = markers(vm, "event")[-1]
        assert last["src"] == "script" and last["value"] == "0", vm.serial
        no_exception(vm)
