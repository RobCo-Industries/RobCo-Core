"""QMP client reply matching, on a socket pair. No QEMU needed."""

import json
import socket
import threading

import pytest

from harness.qemu import Qemu, QemuError


@pytest.fixture
def pair():
    """A Qemu wired to one end of a socket pair; the test plays QEMU on the other."""
    a, b = socket.socketpair()
    q = Qemu("qemu-none", "none", timeout=0.05)
    q._qmp = a
    a.settimeout(q.timeout)
    b.settimeout(1.0)
    yield q, b
    a.close()
    b.close()
    q.close()


def request(server):
    """Read one command line the client sent."""
    buf = b""
    while not buf.endswith(b"\n"):
        buf += server.recv(4096)
    return json.loads(buf)


def send(server, *msgs):
    server.sendall(b"".join(json.dumps(m).encode() + b"\r\n" for m in msgs))


def test_late_reply_is_not_taken_for_the_next_command(pair):
    q, srv = pair
    with pytest.raises(socket.timeout):
        q.execute("query-status")
    first = request(srv)
    send(srv, {"return": "stale", "id": first.get("id")})  # the late answer
    # The next command must get its own reply, not the stale one.
    q._qmp.settimeout(1.0)
    assert run(q, srv, lambda: q.execute("query-name"), reply={"return": "fresh"}) == "fresh"
    # The connection stays in step for the command after that.
    assert run(q, srv, lambda: q.execute("query-name"), reply={"return": "third"}) == "third"


def answer(srv, *before, reply=None, split=None):
    """Read one command, then send `before` messages and the reply for its id."""
    req = request(srv)
    msgs = list(before) + [dict(reply or {"return": {}}, id=req.get("id"))]
    data = b"".join(json.dumps(m).encode() + b"\r\n" for m in msgs)
    if split is None:
        srv.sendall(data)
    else:
        srv.sendall(data[:split])
        srv.sendall(data[split:])
    return req


def run(q, srv, fn, **kw):
    t = threading.Thread(target=answer, args=(srv,) + kw.pop("before", ()), kwargs=kw)
    t.start()
    try:
        return fn()
    finally:
        t.join(2)


def test_event_between_command_and_reply_is_queued(pair):
    q, srv = pair
    q._qmp.settimeout(1.0)
    ev = {"event": "STOP", "data": {}, "timestamp": {"seconds": 1, "microseconds": 0}}
    out = run(q, srv, lambda: q.execute("stop"), before=(ev,), reply={"return": 7})
    assert out == 7
    assert q.events == [ev]
    assert q.wait_event("STOP", timeout=0.05) == ev


def test_reply_split_across_reads(pair):
    q, srv = pair
    q._qmp.settimeout(1.0)
    out = run(q, srv, lambda: q.hmp("info status"),
              reply={"return": "VM status: running\r\n"}, split=10)
    assert out == "VM status: running\r\n"


def test_partial_line_survives_a_timeout(pair):
    q, srv = pair
    with pytest.raises(socket.timeout):
        q.execute("query-status")
    req = request(srv)
    data = json.dumps({"return": "late", "id": req.get("id")}).encode() + b"\r\n"
    srv.sendall(data[:5])
    with pytest.raises(socket.timeout):
        q._qmp_read()
    srv.sendall(data[5:])
    assert q._qmp_read() == {"return": "late", "id": req.get("id")}


def test_error_reply_with_matching_id_raises(pair):
    q, srv = pair
    q._qmp.settimeout(1.0)
    stale = {"error": {"class": "GenericError", "desc": "old"}, "id": "rc-0"}
    with pytest.raises(QemuError, match=r"^bogus: The command bogus has not been found$"):
        run(q, srv, lambda: q.execute("bogus"), before=(stale,),
            reply={"error": {"class": "CommandNotFound",
                             "desc": "The command bogus has not been found"}})


def test_two_stale_replies_in_a_row(pair):
    q, srv = pair
    ids = []
    for _ in range(2):
        with pytest.raises(socket.timeout):
            q.execute("query-status")
        ids.append(request(srv).get("id", len(ids)))
    assert ids[0] != ids[1]
    q._qmp.settimeout(1.0)
    stale = [{"return": f"stale{i}", "id": i_} for i, i_ in enumerate(ids)]
    assert run(q, srv, lambda: q.execute("query-name"), before=tuple(stale),
               reply={"return": "fresh"}) == "fresh"
    assert q.events == []
