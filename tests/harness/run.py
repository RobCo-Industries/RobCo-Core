"""Boot an image in QEMU like boot() does, with the console on this terminal.

    python3 -m harness.run IMAGE --board BOARD [--sd SD.IMG] [--mode auto|svc]
                           [--serial2 none|pty|tcp:PORT] [--qemu-arg ARG]...

Run it with RobCo-Core's tests/ directory on PYTHONPATH. The guest's
PL011 console is this terminal, in raw mode while QEMU runs and restored
on exit. Press Ctrl-A, then x, to quit. Ctrl-A, then c, switches to the
QEMU monitor and back. --serial2 exposes the mini UART, the script
channel, on a pty (QEMU prints its path) or on a TCP port on 127.0.0.1.
The exit status is QEMU's.
"""

import argparse
import contextlib
import os
import subprocess
import sys

from .boot import BOARDS, boot_args

try:
    import termios
except ImportError:  # not a POSIX system
    termios = None


@contextlib.contextmanager
def saved_terminal(fd):
    """Restores the terminal settings of fd on exit, if fd is a terminal."""
    saved = None
    if termios is not None and os.isatty(fd):
        saved = termios.tcgetattr(fd)
    try:
        yield
    finally:
        if saved is not None:
            termios.tcsetattr(fd, termios.TCSADRAIN, saved)


def serial2_args(spec):
    if spec == "none":
        return [], None
    if spec == "pty":
        return ["-serial", "pty"], "serial2: pty, QEMU prints its path below"
    if spec.startswith("tcp:") and spec[4:].isdigit():
        port = int(spec[4:])
        return (["-serial", f"tcp:127.0.0.1:{port},server=on,wait=off"],
                f"serial2: tcp 127.0.0.1:{port}")
    raise ValueError(f"--serial2 takes none, pty or tcp:PORT, not {spec!r}")


def command(image, board, mode="auto", sd=None, serial2="none", extra=()):
    """The QEMU command line: the console on stdio, then boot()'s arguments."""
    s2, _ = serial2_args(serial2)
    cmd = [BOARDS[board].binary, "-M", board, "-display", "none", "-no-user-config",
           "-serial", "mon:stdio"] + s2
    return cmd + boot_args(image, board, mode, sd) + list(extra)


def main(argv=None):
    p = argparse.ArgumentParser(prog="python3 -m harness.run",
                                description=__doc__.splitlines()[0])
    p.add_argument("image")
    p.add_argument("--board", required=True, choices=list(BOARDS))
    p.add_argument("--sd", help="SD card image")
    p.add_argument("--mode", default="auto", choices=["auto", "svc", "hyp"])
    p.add_argument("--serial2", default="none", help="none, pty or tcp:PORT")
    p.add_argument("--qemu-arg", action="append", default=[], help="extra QEMU argument")
    args = p.parse_args(argv)
    try:
        cmd = command(args.image, args.board, args.mode, args.sd, args.serial2, args.qemu_arg)
        _, note = serial2_args(args.serial2)
    except ValueError as e:
        p.error(str(e))
    proc = None
    with saved_terminal(sys.stdin.fileno()):
        try:
            proc = subprocess.Popen(cmd)
            print(f"harness.run: {args.board}, qemu pid {proc.pid}, quit with Ctrl-A x",
                  file=sys.stderr, flush=True)
            if note:
                print(f"harness.run: {note}", file=sys.stderr, flush=True)
            return proc.wait()
        except KeyboardInterrupt:
            return 130
        finally:
            if proc is not None and proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(3)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()


if __name__ == "__main__":
    sys.exit(main())
