"""The harness only parses complete serial lines. No QEMU needed."""

import threading

from harness.boot import Vm, complete_lines, parse_markers

FULL = "@RC stage=input ev=event type=key code=up value=1 src=con\r\n"
PARSED = {"stage": "input", "ev": "event", "type": "key", "code": "up", "value": "1",
          "src": "con"}


class FakeVm(Vm):
    """A Vm with a fixed capture and no sockets or QEMU."""

    def __init__(self, *bufs):
        self._bufs = [bytearray(b) for b in bufs]
        self._cond = threading.Condition()


def test_complete_line_parse_unchanged():
    assert parse_markers(FULL) == [PARSED]
    assert parse_markers(FULL.replace("\r\n", "\n")) == [PARSED]


def test_mid_line_tail_is_ignored():
    text = FULL + "@RC stage=input ev=event type=key"
    assert parse_markers(text) == [PARSED]
    assert complete_lines(text) == FULL


def test_tail_split_inside_crlf_is_ignored():
    text = FULL + FULL[:-1]
    assert text.endswith("\r")
    assert parse_markers(text) == [PARSED]
    assert all(not v.endswith("\r") for m in parse_markers(text) for v in m.values())


def test_empty_tail_and_empty_text():
    assert parse_markers(FULL + FULL) == [PARSED, PARSED]
    assert parse_markers("") == []
    assert parse_markers("@RC stage=x ev=y") == []
    assert complete_lines("no newline yet") == ""


def test_non_marker_lines_are_skipped():
    text = "booting\r\n" + FULL + "|screen row\r\n" + FULL + "half a line"
    assert parse_markers(text) == [PARSED, PARSED]


def test_equals_inside_value():
    m = parse_markers("@RC stage=s ev=cfg line=a=b=c\r\n")
    assert m == [{"stage": "s", "ev": "cfg", "line": "a=b=c"}]


def test_vm_markers_and_lines_skip_partial_tail():
    vm = FakeVm(FULL.encode() + b"@RC stage=input ev=ev", b"ok\r\nerr full\r\nerr")
    assert vm.markers() == [PARSED]
    assert vm.lines(port=1) == ["ok", "err full"]
    assert vm.serial2 == "ok\nerr full\nerr"
    assert vm.serial.endswith("ev=ev")


def test_wait_for_line_mode():
    vm = FakeVm(b"@RC stage=a ev=ready\r\n@RC stage=a ev=done")
    assert vm.wait_for("ev=done") == "ev=done"
    assert vm.wait_for("ev=ready", line=True) == "ev=ready"
