"""The marker table in BOOT_PROTOCOL.md matches what examples/chain1 and chain2 print."""

import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = ROOT / "docs/spec/BOOT_PROTOCOL.md"
SOURCES = [ROOT / "examples/chain1/main.c", ROOT / "examples/chain2/main.c"]

# Events the chain examples print that the spec lists.
CHAIN_EVENTS = ("boot", "handoff", "mount", "load", "fallback", "error", "jump", "ready",
                "shell", "halt")


def spec_keys():
    """{event: [keys]} from the Events table."""
    text = SPEC.read_text()
    table = text[text.index("### Events"):text.index("| Stage | Must emit |")]
    keys = {}
    for m in re.finditer(r"^\| `(\w+)` \| ([^|]*)\|", table, re.M):
        cell = m.group(2).strip().strip("`")
        keys[m.group(1)] = [k.split("=")[0] for k in cell.split()] if cell else []
    return keys


def printed():
    """{event: [format strings]} from the k_printf calls of the chain examples."""
    out = {}
    for src in SOURCES:
        text = re.sub(r'"\s*\n\s*"', "", src.read_text())  # join split string literals
        text = re.sub(r'"\s*[A-Z][A-Z0-9_]*\s*"', "X", text)  # and macros between them
        for m in re.finditer(r'ev=(\w+)([^"]*)', text):
            out.setdefault(m.group(1), []).append(m.group(2))
    return out


def test_chain_events_are_specified():
    keys = spec_keys()
    for ev in printed():
        if ev in CHAIN_EVENTS:
            assert ev in keys, f"ev={ev} is printed but not in the spec"


def test_spec_keys_are_printed():
    """Some line of each event prints every key the spec lists for it."""
    keys = spec_keys()
    got = printed()
    for ev in CHAIN_EVENTS:
        assert ev in keys, ev
        assert ev in got, f"ev={ev} is specified but neither chain example prints it"
        assert any(all(re.search(rf"\b{k}=", f) for k in keys[ev]) for f in got[ev]), \
            f"no ev={ev} line prints all of {keys[ev]}"
