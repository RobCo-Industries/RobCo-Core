"""The examples in the Studio specs are valid under the rules the specs state.

Covers docs/spec/STUDIO_CLI.md, STUDIO_PROJECT.md and SDK_PACKAGE.md: the
JSON lines of each command, the error code table, the scenario files, the
manifests, the package files and the runner ROM's marker lines. No tool
reads these formats yet, so this test is the only reader of the examples.
"""

import json
import pathlib
import re

import pytest

tomllib = pytest.importorskip("tomllib")

SPEC = pathlib.Path(__file__).resolve().parents[2] / "docs" / "spec"
CLI = (SPEC / "STUDIO_CLI.md").read_text(encoding="utf-8")
PROJECT = (SPEC / "STUDIO_PROJECT.md").read_text(encoding="utf-8")
PACKAGE = (SPEC / "SDK_PACKAGE.md").read_text(encoding="utf-8")

# The keys of each kind of JSON line, after "v" and "kind", in table order.
LINE_KEYS = {
    "step": ["step", "item", "state"],
    "diagnostic": ["program", "text"],
    "item": ["type", "name", "version", "host", "installed", "offered"],
    "serial": ["t_ms", "text"],
    "scenario": ["name", "state", "board", "outcome", "failed_step", "updated", "ms"],
    "check": ["scenario", "index", "action", "ok", "reason", "detail", "ms"],
    "error": ["code", "detail", "exit"],
}
PIXEL_KEYS = ["x", "y", "want", "got", "count"]
# The keys a result line adds after "command", "ok" and "exit".
RESULT_OK_KEYS = {
    "sdk list": ["count"],
    "sdk install": ["packages", "tools"],
    "new": ["template", "name", "path", "files"],
    "update": [],
    "build": ["tape", "elf", "size", "sha256", "abi", "gcc", "sources"],
}
RESULT_ALWAYS_KEYS = {
    "run": ["outcome", "board", "line", "tape_result", "tape_rc", "ms"],
    "test": ["scenarios", "passed", "failed", "updated"],
}
STEPS = {"resolve", "download", "verify", "install", "compile", "link", "objcopy", "seal",
         "card", "boot"}
ACTIONS = {"expect": str, "press": str, "release": str, "tap": str, "dial": int,
           "sleep_ms": int, "screen": str, "exit": int}
KEY_NAMES = {"up", "down", "left", "right", "select", "back", "dial_push"}
NAME_RE = re.compile(r"[a-z0-9_-]{1,32}")
CODE_PREFIXES = ("usage", "project", "manifest", "lock", "new", "run", "test", "package",
                 "index", "sdk", "tool", "firmware", "cache", "templates", "build", "sim",
                 "guest", "io", "internal")


def blocks(text, lang):
    """The fenced blocks of `text` whose info string is `lang`, each ending in a line feed."""
    out = []
    current = None
    for line in text.split("\n"):
        if current is None:
            if line.startswith("```"):
                current = (line[3:].strip(), [])
        elif line == "```":
            if current[0] == lang:
                out.append("".join(x + "\n" for x in current[1]))
            current = None
        else:
            current[1].append(line)
    assert current is None, "a fence is left open"
    return out


def error_codes():
    """{code: exit} from the table of "Error codes" in STUDIO_CLI.md."""
    table = CLI[CLI.index("### Error codes"):CLI.index("## JSON lines")]
    codes = {}
    for cell, exit_code in re.findall(r"^\| (`[^|]*) \| (\d+) \|", table, re.M):
        for code in re.findall(r"`([^`]+)`", cell):
            assert code not in codes, f"{code} is listed twice"
            codes[code] = int(exit_code)
    return codes


def exit_codes():
    table = CLI[CLI.index("## Exit codes"):CLI.index("### Error codes")]
    return {int(n) for n in re.findall(r"^\| (\d+) \| `", table, re.M)}


def json_line_runs():
    """Each json block of STUDIO_CLI.md as a list of (raw line, object)."""
    runs = []
    for block in blocks(CLI, "json"):
        lines = [x for x in block.split("\n") if x]
        runs.append([(x, json.loads(x)) for x in lines])
    assert len(runs) >= 10
    return runs


def test_error_code_table_is_complete_and_consistent():
    codes = error_codes()
    exits = exit_codes()
    assert exits == set(range(11))
    assert set(codes.values()) <= exits
    for code in codes:
        assert code.split(":")[0] in CODE_PREFIXES, code
    # Every code the three specs name in backticks is in the table.
    named = set()
    for text in (CLI, PROJECT, PACKAGE):
        for code in re.findall(r"`((?:%s): [a-z][a-z ]*[a-z])`" % "|".join(CODE_PREFIXES), text):
            named.add(code)
    assert named, "the code pattern matches nothing"
    assert named - set(codes) == set()


def test_every_json_line_follows_its_table():
    for run in json_line_runs():
        for raw, obj in run:
            keys = list(obj)
            assert keys[:2] == ["v", "kind"], raw
            assert obj["v"] == 1, raw
            assert "\n" not in raw and "\r" not in raw
            kind = obj["kind"]
            rest = keys[2:]
            if kind == "result":
                assert rest[:3] == ["command", "ok", "exit"], raw
                command = obj["command"]
                assert obj["ok"] is (obj["exit"] == 0), raw
                if command in RESULT_ALWAYS_KEYS:
                    assert rest[3:] == RESULT_ALWAYS_KEYS[command], raw
                else:
                    assert rest[3:] == (RESULT_OK_KEYS[command] if obj["ok"] else []), raw
            elif kind == "check" and obj["reason"] == "pixels":
                assert rest == LINE_KEYS["check"] + PIXEL_KEYS, raw
                assert re.fullmatch(r"#[0-9a-f]{6}", obj["want"]), raw
                assert re.fullmatch(r"#[0-9a-f]{6}", obj["got"]), raw
            else:
                assert rest == LINE_KEYS[kind], raw
            if kind == "step":
                assert obj["step"] in STEPS and obj["state"] in ("start", "done", "skip"), raw
            for value in obj.values():
                assert not isinstance(value, float), raw


def test_every_run_ends_with_one_result_and_names_its_error():
    codes = error_codes()
    for run in json_line_runs():
        objs = [o for _, o in run]
        assert [o["kind"] for o in objs].count("result") == 1
        result = objs[-1]
        assert result["kind"] == "result"
        errors = [o for o in objs if o["kind"] == "error"]
        if result["exit"] == 0:
            assert errors == []
            continue
        assert errors == [objs[-2]], "one error line, right before the result"
        error = errors[0]
        assert error["exit"] == result["exit"]
        assert "\n" not in error["detail"]
        wanted = codes[error["code"]]
        if error["code"] == "test: failed":
            assert error["exit"] in (1, 7)
        else:
            assert error["exit"] == wanted, error


def test_steps_open_and_close_in_order():
    for run in json_line_runs():
        open_step = None
        for raw, obj in run:
            if obj["kind"] != "step":
                continue
            ident = (obj["step"], obj["item"])
            if obj["state"] == "start":
                assert open_step is None, raw
                open_step = ident
            elif obj["state"] == "done":
                # A block that shows only the end of a run may start inside a step.
                assert open_step in (ident, None), raw
                open_step = None
            else:
                assert open_step is None, raw


def test_test_command_examples_add_up():
    for run in json_line_runs():
        result = run[-1][1]
        if result["command"] != "test":
            continue
        assert result["passed"] + result["failed"] == result["scenarios"]
        done = [o for _, o in run if o["kind"] == "scenario" and o["state"] == "done"]
        assert len(done) == result["scenarios"]
        assert sum(o["outcome"] == "passed" for o in done) == result["passed"]
        for o in done:
            failed = [c for _, c in run if c["kind"] == "check" and c["scenario"] == o["name"]
                      and not c["ok"]]
            assert o["failed_step"] == (failed[0]["index"] if failed else 0)


def check_scenario(doc):
    assert set(doc) <= {"format", "timeout_ms", "step"}
    assert doc["format"] == 1
    assert 1000 <= doc.get("timeout_ms", 120000) <= 3600000
    steps = doc["step"]
    assert 1 <= len(steps) <= 256
    for i, step in enumerate(steps):
        actions = [k for k in step if k in ACTIONS]
        assert len(actions) == 1, step
        action = actions[0]
        value = step[action]
        assert type(value) is ACTIONS[action], step
        assert set(step) <= {action, "timeout_ms"}, step
        if "timeout_ms" in step:
            assert action in ("expect", "exit") and 1 <= step["timeout_ms"] <= 600000, step
        if action in ("press", "release", "tap"):
            assert value in KEY_NAMES, step
        elif action == "dial":
            assert value != 0 and -1000000 <= value <= 1000000, step
        elif action == "sleep_ms":
            assert 1 <= value <= 60000, step
        elif action == "screen":
            assert NAME_RE.fullmatch(value), step
        elif action == "expect":
            data = value.encode()
            assert 1 <= len(data) <= 200 and all(0x20 <= b != 0x7F for b in data), step
        elif action == "exit":
            assert 0 <= value <= 255 and i == len(steps) - 1, step
    return steps


def test_scenario_examples_follow_the_step_rules():
    scenarios = [tomllib.loads(b) for b in blocks(CLI, "toml") + blocks(PACKAGE, "toml")
                 if "[[step]]" in b]
    assert len(scenarios) == 2
    used = set()
    for doc in scenarios:
        used |= {k for step in check_scenario(doc) for k in step if k in ACTIONS}
    assert used == set(ACTIONS), "the examples show every kind of step"


def test_manifest_examples_parse_and_hold_only_known_keys():
    known = {"project": {"name", "title", "version", "kind", "subtype", "icon"},
             "target": {"platform", "abi"}, "build": {"backend", "sources"}}
    manifests = [tomllib.loads(b) for b in blocks(PROJECT, "toml") + blocks(PACKAGE, "toml")
                 if "[project]" in b]
    assert len(manifests) == 3
    names = set()
    for doc in manifests:
        assert set(doc) == {"format", "project", "target", "build"}
        assert doc["format"] == 1
        for table, keys in known.items():
            assert set(doc[table]) <= keys, doc[table]
        assert doc["build"]["sources"]
        names.add(doc["project"]["name"])
    # The template's manifest holds the token, the other two a real name.
    assert names == {"PIPES", "CULVERT", "@NAME@"}
    culvert = next(d for d in manifests if d["project"]["name"] == "CULVERT")
    assert culvert["build"]["sources"] == ["main.c", "game.c", "logic.c", "draw.c"]


def test_template_tokens_are_only_name():
    template = next(b for b in blocks(PACKAGE, "toml") if "@NAME@" in b)
    assert set(re.findall(r"@[A-Z_]+@", template)) == {"@NAME@"}
    filled = tomllib.loads(template.replace("@NAME@", "PIPES"))
    assert filled["project"]["name"] == "PIPES"


def package_json(marker):
    found = [json.loads(b) for b in blocks(PACKAGE, "json") if marker in b]
    assert len(found) == 1, marker
    return found[0]


def test_sdk_json_and_index_name_the_same_tools():
    sdk = package_json('"built_from"')
    index = package_json('"packages"')
    tools = sdk["tools"]
    assert [t["name"] for t in tools] == ["gcc", "qemu"]
    assert all(set(t) == {"name", "version"} for t in tools)
    assert index["packages"][0]["tools"] == tools
    offered = {(t["name"], t["version"]) for t in index["tools"]}
    assert {(t["name"], t["version"]) for t in tools} <= offered
    assert sdk["formats"] == {"rimg": 1, "flags": 1, "boot": 1, "runner": 1}
    assert [f["path"] for f in sdk["files"]] == sorted(f["path"] for f in sdk["files"])


def section(text, heading, level="###"):
    """The text of one section, from its heading to the next heading of that level or above."""
    start = text.index(f"{level} {heading}\n")
    end = min((i for i in (text.find("\n" + "#" * n + " ", start + 1)
                           for n in range(2, len(level) + 1)) if i != -1), default=len(text))
    return text[start:end]


def test_sdk_json_lists_the_files_of_the_layout_table():
    layout = section(PACKAGE, "Layout of pipos")
    first, second = layout.split("The headers are the whole of")[:2]
    paths = set(re.findall(r"^\| `([^`]+)` \|", first, re.M))
    headers = set(re.findall(r"^\| `([a-z0-9]+\.h)` \|", second, re.M))
    assert len(headers) == 10 and "pip.h" in headers and "pipos.h" not in headers
    assert "include/pipos/*.h" in paths
    paths = (paths - {"include/pipos/*.h", "sdk.json"}) | {f"include/pipos/{h}" for h in headers}
    sdk = package_json('"built_from"')
    listed = [f["path"] for f in sdk["files"]]
    assert set(listed) == paths and len(listed) == len(paths)
    assert listed == sorted(listed, key=str.encode), "files is sorted by path in byte order"
    assert all(re.fullmatch(r"[0-9a-f]{64}", f["sha256"]) and f["size"] > 0 for f in sdk["files"])
    # Every path of flags.json is a file of the package.
    flags = package_json('"cflags"')
    for path in [flags["crt0"], *flags["libs"], *flags["linker_scripts"].values()]:
        assert path in paths, path
    assert all(any(p.startswith(d + "/") for p in paths) for d in flags["include_dirs"])
    # The header the template includes ships in the package.
    includes = set(re.findall(r"includes `<(pipos/[a-z0-9]+\.h)>`", PACKAGE))
    assert includes == {"pipos/pip.h"}
    assert all(f"include/{i}" in paths for i in includes)


def test_lock_example_pins_the_tools_the_package_names():
    lock = next(tomllib.loads(b) for b in blocks(PROJECT, "toml") if "[[package]]" in b)
    sdk = package_json('"built_from"')
    assert {t["name"]: t["version"] for t in lock["tool"]} == \
        {t["name"]: t["version"] for t in sdk["tools"]}


def test_templates_json_example_is_sorted_and_matches_the_file_table():
    doc = package_json('"templates"')
    assert doc["format"] == 1
    ids = [t["id"] for t in doc["templates"]]
    assert ids == sorted(ids) == ["holotape-c"]
    for t in doc["templates"]:
        assert set(t) == {"id", "title", "description", "kind", "files"}
        assert re.fullmatch(r"[a-z][a-z0-9-]{0,31}", t["id"])
        paths = [f["path"] for f in t["files"]]
        assert paths == sorted(paths, key=str.encode)
        assert all(set(f) == {"path", "substitute"} for f in t["files"])
    files = {f["path"]: f["substitute"] for f in doc["templates"][0]["files"]}
    assert files == {"icon.pbm": False, "robco.toml": True, "src/main.c": False,
                     "tests/golden/start/ready.png": False, "tests/start.toml": False}
    # The new command's example lists the same files.
    new = next(o for run in json_line_runs() for _, o in run
               if o["kind"] == "result" and o["command"] == "new" and o["ok"])
    assert new["files"] == sorted(files)


RUNNER_LINE = re.compile(
    r"@RC stage=runner ev=(?:run tape=[!-~]{5,12}"
    r"|done result=(?:exit rc=(?:\d|[1-9]\d|1\d\d|2[0-4]\d|25[0-5])"
    r"|fault rc=-14|missing rc=-2|bad rc=-40|io rc=-5|eject rc=-41|error rc=-\d+)"
    r"|done result=config rc=-?\d+ why=(?:open|read|size|parse|name))")


def test_runner_marker_examples_have_the_exact_form():
    lines = [x for text in (PACKAGE, CLI) for x in re.findall(r"@RC stage=runner [^\n\"]*", text)
             if "<" not in x and "`" not in x]
    assert len(lines) >= 8
    for line in lines:
        assert RUNNER_LINE.fullmatch(line), line


def test_card_config_text_is_stable():
    """The simulator card's PIPOS.CFG and RUNNER.CFG, as the spec prints them."""
    cfg = next(b for b in blocks(CLI, "") if b.startswith("[display]"))
    assert cfg.count("\n") == 11 and len(cfg) == 113 and "\r" not in cfg
    keys = re.findall(r"^(\w+) = (\w+)$", cfg, re.M)
    assert keys == [("scheme", "green"), ("line_delay_ms", "0"), ("backend", "hdmi"),
                    ("volume", "100"), ("beep", "no"), ("slot", "off")]
    runner = next(b for b in blocks(PACKAGE, "") if b.startswith("[run]"))
    assert runner == "[run]\ntape = NAME.HTP\n"


# The grammar of "What the runner accepts", as a model of pip_ini_get().
INI_SYNTAX, INI_TOO_LONG, INI_CHAR, INI_NOT_FOUND, INI_SPACE = -2, -3, -4, -5, -6
INI_NAME = set(b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-")
INI_BLANK = set(b" \t")


def ini_get(text, section, key, out_size):
    """(code, value) for the last `key` of `section`, by the rules of the spec's table."""

    def blank_rest(rest):
        rest = rest.lstrip(b" \t")
        return rest == b"" or rest[:1] in (b"#", b";")

    found, current = None, b""
    for line in text.split(b"\n")[:-1] if text.endswith(b"\n") else text.split(b"\n"):
        if line.endswith(b"\r"):
            line = line[:-1]
        if any((b < 0x20 and b != 9) or b == 0x7F for b in line):
            return INI_CHAR, b""
        line = line.lstrip(b" \t")
        if line == b"" or line[:1] in (b"#", b";"):
            continue
        if line[:1] == b"[":
            close = line.find(b"]")
            body = line[1:close] if close != -1 else b""
            if close == -1 or not set(body) <= INI_NAME | INI_BLANK:
                return INI_SYNTAX, b""
            name = body.strip(b" \t")
            if name == b"":
                return INI_SYNTAX, b""
            if len(name) > 31:
                return INI_TOO_LONG, b""
            if not blank_rest(line[close + 1:]):
                return INI_SYNTAX, b""
            current = name
            continue
        n = 0
        while n < len(line) and line[n] in INI_NAME:
            n += 1
        if n > 31:
            return INI_TOO_LONG, b""
        rest = line[n:].lstrip(b" \t")
        if n == 0 or rest[:1] != b"=":
            return INI_SYNTAX, b""
        rest = rest[1:].lstrip(b" \t")
        if rest[:1] == b'"':
            value, i = bytearray(), 1
            while True:
                if i == len(rest):
                    return INI_SYNTAX, b""
                c = rest[i:i + 1]
                i += 1
                if c == b'"':
                    break
                if c == b"\\":
                    c = {b"n": b"\n", b'"': b'"', b"\\": b"\\"}.get(rest[i:i + 1])
                    i += 1
                    if c is None:
                        return INI_SYNTAX, b""
                value += c
            if len(value) > 127:
                return INI_TOO_LONG, b""
            if not blank_rest(rest[i:]):
                return INI_SYNTAX, b""
            value = bytes(value)
        else:
            stop = len(rest)
            for i, c in enumerate(rest):
                if c in b"#;" and (i == 0 or rest[i - 1] in INI_BLANK):
                    stop = i
                    break
            value = rest[:stop].rstrip(b" \t")
            if len(value) > 127:
                return INI_TOO_LONG, b""
        if current == section and line[:n] == key:
            found = value
    if found is None:
        return INI_NOT_FOUND, b""
    if len(found) >= out_size:
        return INI_SPACE, b""
    return 0, found


def runner_config(data):
    """What the runner makes of RUNNER.CFG: ("run", name) or ("config", rc, why)."""
    if not 1 <= len(data) <= 255:
        return ("config", 0, "size")
    code, value = ini_get(data, b"run", b"tape", 13)
    if code != 0:
        return ("config", code, "parse")
    if not 1 <= len(value) <= 12 or not all(0x21 <= b <= 0x7E for b in value):
        return ("config", 0, "name")
    return ("run", value)


def test_runner_config_grammar_and_failure_table():
    written = next(b for b in blocks(PACKAGE, "") if b.startswith("[run]"))
    for name in ("A", "PIPES", "CULVERT", "ABCD_-78"):
        data = written.replace("NAME", name).encode()
        assert 19 <= len(data) <= 26
        assert runner_config(data) == ("run", f"{name}.HTP".encode())
    cases = {
        b"": (0, "size"),
        b"#" * 256: (0, "size"),
        b"[run]\ntape = A.HTP\x00\n": (INI_CHAR, "parse"),
        b"[run]\rtape = A.HTP\n": (INI_CHAR, "parse"),
        b"\xef\xbb\xbf[run]\ntape = A.HTP\n": (INI_SYNTAX, "parse"),
        b"[run]\nnonsense\n": (INI_SYNTAX, "parse"),
        b"[run\ntape = A.HTP\n": (INI_SYNTAX, "parse"),
        b"[run]\ntape = \"A.HTP\n": (INI_SYNTAX, "parse"),
        b"[run]\ntape = \"A\\x.HTP\"\n": (INI_SYNTAX, "parse"),
        b"[run]\ntape = \"A.HTP\" x\n": (INI_SYNTAX, "parse"),
        b"[" + b"s" * 32 + b"]\n": (INI_TOO_LONG, "parse"),
        b"[run]\n" + b"k" * 32 + b" = 1\n": (INI_TOO_LONG, "parse"),
        b"[run]\ntape = " + b"v" * 128 + b"\n": (INI_TOO_LONG, "parse"),
        b"[other]\ntape = A.HTP\n": (INI_NOT_FOUND, "parse"),
        b"tape = A.HTP\n[run]\n": (INI_NOT_FOUND, "parse"),
        b"[Run]\ntape = A.HTP\n": (INI_NOT_FOUND, "parse"),
        b"[run]\nTape = A.HTP\n": (INI_NOT_FOUND, "parse"),
        b"[run]\ntape = ABCDEFGH.HTPX\n": (INI_SPACE, "parse"),
        b"[run]\ntape = A.HTP\ntape = ABCDEFGH.HTPX\n": (INI_SPACE, "parse"),
        b"[run]\ntape =\n": (0, "name"),
        b"[run]\ntape = A B.HTP\n": (0, "name"),
        b"[run]\ntape = \"A\\n.HTP\"\n": (0, "name"),
        b"[run]\ntape = A\xe9.HTP\n": (0, "name"),
    }
    for data, (rc, why) in cases.items():
        assert runner_config(data) == ("config", rc, why), data
    accepted = {
        b"[run]\r\ntape=A.HTP": b"A.HTP",
        b"  [ run ]  ; the tape\n\ttape\t=\tA.HTP   # comment\n": b"A.HTP",
        b"[run]\ntape = A#B.HTP\n": b"A#B.HTP",
        b"[run]\ntape = \"A.HTP\"\n": b"A.HTP",
        b"[run]\ntape = OLD.HTP\n[x]\nk = v\n[run]\ntape = NEW.HTP\n": b"NEW.HTP",
        b"[run]\ntape = ABCDEFGH.HTP\n": b"ABCDEFGH.HTP",
    }
    for data, name in accepted.items():
        assert runner_config(data) == ("run", name), data
    # The failure table names each code the model can give, and no other.
    table = section(PACKAGE, "What the runner accepts", "####")
    table = table[table.index("Each way the file can be wrong"):]
    pairs = set(re.findall(r"`rc=(-?\d+) why=([a-z]+)`", table))
    assert pairs == {("-2", "open"), ("0", "size"), ("-4", "parse"), ("-2", "parse"), ("-3", "parse"),
                     ("-5", "parse"), ("-6", "parse"), ("0", "name")}
    assert {(str(rc), why) for rc, why in cases.values()} == pairs - {("-2", "open")}
