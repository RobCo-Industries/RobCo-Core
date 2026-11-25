"""Write boot.json from the harness's own board table.

boot.json holds the arguments of boot_args() as data, so a tool other
than this harness boots a board the same way (docs/spec/TESTING.md,
"boot.json"). Every value comes from boot_args(), called with the
placeholders in place of the paths, so the file and the harness cannot
disagree.

Usage, from tests/:

    python -m harness.bootjson            # print boot.json
    python -m harness.bootjson -o FILE    # write it to FILE
"""

import argparse
import json
import re
import sys

from .boot import BOARDS, boot_args
from .qemu import COMMON_ARGS

FORMAT = 1
STUB = "{stub}"
IMAGE = "{image}"
SD = "{sd}"


def modes(board):
    """The modes `board` can boot, in the order boot.json lists them."""
    return ["svc"] if BOARDS[board].auto_mode == "svc" else ["svc", "hyp"]


def _sd_args():
    """The arguments boot_args() adds for an SD image, the same on every board."""
    found = None
    for board in BOARDS:
        for mode in modes(board):
            bare = boot_args(IMAGE, board, mode, stub=STUB)
            full = boot_args(IMAGE, board, mode, sd=SD, stub=STUB)
            if full[:len(bare)] != bare:
                raise ValueError(f"{board} {mode}: the SD arguments are not a suffix")
            tail = full[len(bare):]
            if found is not None and tail != found:
                raise ValueError(f"{board} {mode}: the SD arguments differ between boards")
            found = tail
    return found


PLACEHOLDERS = (STUB, IMAGE, SD)
PLACEHOLDER_RE = re.compile(r"\{[^{}]*\}")


def validate(doc):
    """Raise ValueError if `doc` breaks a rule a reader enforces.

    format is exactly 1, each default_mode is a listed mode, strings hold
    only the listed placeholders, and {sd} appears only in "sd", so a
    boot without an SD image never meets it.
    """
    if doc.get("format") != FORMAT or type(doc.get("format")) is not int:
        raise ValueError(f"format must be {FORMAT}, not {doc.get('format')!r}")

    def check(strings, where, allowed):
        for s in strings:
            for p in PLACEHOLDER_RE.findall(s):
                if p not in allowed:
                    raise ValueError(f"{where}: placeholder {p} not allowed in {s!r}")

    check(doc["common"], "common", ())
    check(doc["sd"], "sd", (SD,))
    for name, b in doc["boards"].items():
        if b["default_mode"] not in b["modes"]:
            raise ValueError(f"{name}: default_mode {b['default_mode']!r} is not listed")
        for mode, args in b["modes"].items():
            check(args, f"{name} {mode}", (STUB, IMAGE))


def build():
    """boot.json as a dict."""
    boards = {}
    for name, b in BOARDS.items():
        boards[name] = {
            "qemu": b.binary,
            "machine": name,
            "default_mode": b.auto_mode,
            "modes": {m: boot_args(IMAGE, name, m, stub=STUB) for m in modes(name)},
        }
    doc = {"format": FORMAT, "common": list(COMMON_ARGS), "sd": _sd_args(),
           "boards": boards}
    validate(doc)
    return doc


def dumps():
    """boot.json as text: two-space indent, a final newline."""
    return json.dumps(build(), indent=2) + "\n"


def main(argv=None):
    p = argparse.ArgumentParser(description="Print boot.json from the harness's board table.")
    p.add_argument("-o", "--output", help="write to this file instead of stdout")
    a = p.parse_args(argv)
    text = dumps()
    if a.output:
        with open(a.output, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
