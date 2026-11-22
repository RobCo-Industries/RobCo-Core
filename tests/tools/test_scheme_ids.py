"""The console scheme ids in console.h match con_scheme in BOOT_PROTOCOL.md."""

import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def read(*parts):
    with open(os.path.join(ROOT, *parts), encoding="utf-8") as f:
        return f.read()


def test_scheme_ids_match_spec():
    header = read("include", "robco", "console.h")
    ids = {name.lower(): int(v) for name, v in
           re.findall(r"#define CONSOLE_SCHEME_(GREEN|AMBER|BLUE|WHITE)\s+(\d+)", header)}
    row = re.search(r"\| `con_scheme` \|[^|]*: ([^|]+)\|", read("docs", "spec", "BOOT_PROTOCOL.md"))
    assert row, "con_scheme row not found"
    spec = {name: int(v) for v, name in re.findall(r"(\d+) (\w+)", row.group(1))}
    assert len(ids) == 4
    assert spec == ids
