"""The examples in the Studio specs are valid under the rules the specs state.

Covers docs/spec/STUDIO_CLI.md, STUDIO_PROJECT.md and SDK_PACKAGE.md: the
JSON lines of each command, the error code table and the order of checks,
the scenario files, the manifests, the lock and its writer, the package
files, the index and its resolver, RUNNER.CFG and the runner ROM's marker
lines with the key of a run. It also holds the boot.json example of
TESTING.md and the PBM number rule of IMAGE_FORMAT.md to their rules. No
tool reads these formats yet, so this test is the only reader of the
examples.
"""

import json
import pathlib
import re

import pytest

try:
    import tomllib
except ModuleNotFoundError:
    # Python 3.9 and 3.10: tomli is the parser that became tomllib, and
    # tests/requirements.txt installs it there. No test here skips for want of it.
    try:
        import tomli as tomllib
    except ModuleNotFoundError:
        raise ModuleNotFoundError(
            "no TOML parser: run make venv to install tomli from tests/requirements.txt") from None

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
    "": [],
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
                    assert command != "" or obj["exit"] == 2, raw
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


def runs_of(run):
    """The runs of one `robco test` block: (done line, its check lines), told apart by position."""
    runs, checks = [], []
    for _, o in run:
        if o["kind"] == "scenario" and o["state"] == "start":
            checks = []
        elif o["kind"] == "check":
            checks.append(o)
        elif o["kind"] == "scenario":
            assert all(c["scenario"] == o["name"] for c in checks), o
            runs.append((o, checks))
            checks = []
    return runs


def test_test_command_examples_add_up():
    boards_seen = set()
    for run in json_line_runs():
        result = run[-1][1]
        if result["command"] != "test":
            continue
        assert result["passed"] + result["failed"] == result["scenarios"]
        runs = runs_of(run)
        assert len(runs) == result["scenarios"], "one run per scenario and board"
        assert sum(o["outcome"] == "passed" for o, _ in runs) == result["passed"]
        assert sum(o["updated"] for o, _ in runs) == result["updated"]
        assert len({(o["name"], o["board"]) for o, _ in runs}) == len(runs)
        for o, checks in runs:
            failed = [c for c in checks if not c["ok"]]
            assert o["failed_step"] == (failed[0]["index"] if failed else 0)
            if o["outcome"] == "simulator":
                assert all(c["reason"] == "simulator" for c in failed), o
            else:
                assert (o["outcome"] == "passed") == (not failed), o
            boards_seen.add(o["board"])
        # The images of a scenario are written after its last board, and only when every
        # board passed.
        for name in {o["name"] for o, _ in runs}:
            mine = [o for o, _ in runs if o["name"] == name]
            assert all(o["updated"] == 0 for o in mine[:-1]), name
            if any(o["outcome"] != "passed" for o in mine):
                assert mine[-1]["updated"] == 0, name
        for _, o in run:
            if o["kind"] == "scenario" and o["state"] == "start":
                assert (o["outcome"], o["failed_step"], o["updated"], o["ms"]) == ("", 0, 0, 0)
    assert len(boards_seen) >= 2, "an example runs one scenario on two boards"


def test_golden_images_are_shared_by_every_board():
    golden = section(CLI, "Golden images")
    assert "| Path | `tests/golden/SCENARIO/NAME.png`" in golden
    assert "| Boards | One image per screen, shared by every board." in golden
    assert "One image for every board" not in CLI, "the row that read both ways"
    # No golden path of the three specs has a board in it.
    boards = ("raspi0", "raspi1ap", "raspi2b", "raspi3b")
    paths = {m for text in (CLI, PROJECT, PACKAGE)
             for m in re.findall(r"tests/golden/[A-Za-z0-9_/-]+\.png", text)}
    assert len(paths) >= 4
    for path in paths:
        parts = path.split("/")
        assert len(parts) == 4 and not set(parts) & set(boards), path
    # A capture that failed is kept under build/, with its board, so two boards never
    # write one file.
    kept = {m for m in re.findall(r"build/test/[A-Za-z0-9_/-]+\.png", CLI)}
    assert kept and all(len(k.split("/")) == 5 for k in kept), kept
    assert any(k.split("/")[3] in boards for k in kept)
    # The command line of robco test is written the same way in every place.
    form = "robco test [SCENARIO...] [--update-golden] [--board BOARD[,BOARD...]] [--index LOCATION]"
    assert CLI.count(form) == 2 and "[--board BOARD] [--index LOCATION]` | Builds, then runs" not in CLI
    # Each case of the table says what happens with and without --update-golden.
    rows = [r for r in golden.split("\n") if r.startswith("| The ")]
    assert len(rows) == 4 and all(len(r.strip("|").split("|")) == 3 for r in rows)
    assert "test: golden" in rows[0] and "test: failed" in rows[3]


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


def toml_1_0(text):
    """`text` with the forms that only TOML 1.1 has rewritten into TOML 1.0."""
    def one_line(m):
        body = re.sub(r"\s*\n\s*", " ", m[1]).strip()
        return "{ " + body.rstrip(",").strip() + " }"
    text = re.sub(r"\{([^{}]*\n[^{}]*)\}", one_line, text)
    text = re.sub(r"\\x([0-9A-Fa-f]{2})", r"\\u00\1", text)
    return text.replace("\\e", "\\u001B")


def test_toml_1_1_example_means_the_first_manifest():
    for text in (PROJECT, CLI):
        assert "TOML 1.0 in" not in text and "not TOML 1.0" not in text
    plain = next(tomllib.loads(b) for b in blocks(PROJECT, "toml") if "[project]" in b)
    newer = next(b for b in blocks(PROJECT, "toml") if "project = {" in b)
    # It uses forms that only TOML 1.1 has: an inline table over lines, a last comma, \xHH.
    assert re.search(r"\{\n", newer) and re.search(r",\n\}", newer) and "\\x70" in newer
    try:
        parsed = tomllib.loads(newer)
    except tomllib.TOMLDecodeError:
        parsed = tomllib.loads(toml_1_0(newer))    # this Python reads TOML 1.0 only
    assert parsed == plain
    # The order of keys carries no meaning: this example does not start with format.
    assert list(parsed)[0] != "format" and list(plain)[0] == "format"
    assert "first key" not in PROJECT.replace("need not be the first key", "")
    # What the tool writes stays in the forms both versions share.
    for block in blocks(PROJECT, "toml") + blocks(PACKAGE, "toml") + blocks(CLI, "toml"):
        if block != newer:
            assert toml_1_0(block) == block and "\\e" not in block


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
        sources = doc["build"]["sources"]
        assert 1 <= len(sources) <= 256 and all(type(s) is str for s in sources)
        assert all("*" not in s.rsplit("/", 1)[0] for s in sources if "/" in s), \
            "a star only in the last component"
        assert doc["build"].get("backend", "robco") == "robco"
        names.add(doc["project"]["name"])
    # The template's manifest holds the token, the other two a real name.
    assert names == {"PIPES", "CULVERT", "@NAME@"}
    culvert = next(d for d in manifests if d["project"]["name"] == "CULVERT")
    assert culvert["build"]["sources"] == ["main.c", "game.c", "logic.c", "draw.c"]
    # The limit of sources is stated once as a range and once per end.
    build = section(PROJECT, "[build]")
    assert "| 1 to 256 patterns, each a path pattern |" in build
    assert "`sources` holds 1 to 256 patterns." in build
    assert "| `[]` | `manifest: sources`" in build and "| 257 entries or more | `manifest: sources`" in build


def test_template_tokens_are_only_name():
    template = next(b for b in blocks(PACKAGE, "toml") if "@NAME@" in b)
    assert set(re.findall(r"@[A-Z_]+@", template)) == {"@NAME@"}
    filled = tomllib.loads(template.replace("@NAME@", "PIPES"))
    assert filled["project"]["name"] == "PIPES"


def package_json(marker):
    found = [json.loads(b) for b in blocks(PACKAGE, "json") if marker in b]
    assert len(found) == 1, marker
    return found[0]


VERSION = r"(?:0|[1-9]\d{0,4})\.(?:0|[1-9]\d{0,4})\.(?:0|[1-9]\d{0,4})"
HOST = r"[a-z][a-z0-9-]{0,31}"
KNOWN_FORMATS = {"rimg", "flags", "boot", "runner", "templates", "project", "scenario"}


def index_entry(name):
    return next(e for e in package_json('"packages"')["packages"] if e["name"] == name)


def test_sdk_json_and_index_name_the_same_tools():
    sdk = package_json('"firmware"')
    index = package_json('"packages"')
    tools = sdk["tools"]
    assert [t["name"] for t in tools] == ["gcc", "qemu"]
    assert all(set(t) == {"name", "version"} for t in tools)
    assert index_entry("pipos")["tools"] == tools
    offered = {(t["name"], t["version"]) for t in index["tools"]}
    assert {(t["name"], t["version"]) for t in tools} <= offered
    assert sdk["formats"] == {"rimg": 1, "flags": 1, "boot": 1, "runner": 1}
    assert [f["path"] for f in sdk["files"]] == sorted(f["path"] for f in sdk["files"])


def test_sdk_json_examples_equal_their_index_entries():
    """The fields of "What must equal the index entry", and "Which package carries which field"."""
    for marker, name in (('"firmware"', "pipos"), ('"RobCo-Reel"', "templates")):
        sdk = package_json(marker)
        entry = index_entry(name)
        assert sdk["format"] == 1 and sdk["name"] == name
        for field in ("name", "version", "abi", "tools", "formats"):
            assert sdk.get(field) == entry.get(field), (name, field)
        assert sdk.get("requires", []) == entry.get("requires", [])
        assert re.fullmatch(VERSION, sdk["version"])
        assert set(sdk["formats"]) <= KNOWN_FORMATS and set(sdk["formats"].values()) == {1}
        for field in ("abi", "tools", "firmware"):
            assert (field in sdk) == (name == "pipos"), (name, field)
        if name == "pipos":
            assert sdk["abi"] == sdk["version"].rsplit(".", 1)[0]
        paths = [f["path"] for f in sdk["files"]]
        assert all(a.encode() < b.encode() for a, b in zip(paths, paths[1:])), "sorted, none twice"
        assert set(entry) <= {"name", "version", "abi", "requires", "tools", "formats", "url", "size",
                              "sha256"}
        assert {"name", "version", "formats", "url", "size", "sha256"} <= set(entry)
    # The templates package holds the files that templates.json lists, and nothing else.
    templates = package_json('"substitute"')
    listed = {f"{t['id']}/{f['path']}" for t in templates["templates"] for f in t["files"]}
    paths = {f["path"] for f in package_json('"RobCo-Reel"')["files"]}
    assert paths == listed | {"templates.json", "LICENSES/MIT.txt"}
    index = package_json('"packages"')
    keys = [(e["name"], e["version"]) for e in index["packages"]]
    assert len(keys) == len(set(keys))
    for tool in index["tools"]:
        assert tool["name"] in ("gcc", "qemu")
        assert re.fullmatch(r'[!-~]{1,64}', tool["version"]) and not set('"\\') & set(tool["version"])
        for host, archive in tool["archives"].items():
            assert re.fullmatch(HOST, host), host
            assert set(archive) == {"url", "size", "sha256", "root"}


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
    sdk = package_json('"firmware"')
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
    sdk = package_json('"firmware"')
    assert {t["name"]: t["version"] for t in lock["tool"]} == \
        {t["name"]: t["version"] for t in sdk["tools"]}


def test_templates_json_example_is_sorted_and_matches_the_file_table():
    doc = package_json('"substitute"')
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


KEY = r"[0-7][0-9a-f]{7}[0-7][0-9a-f]{7}"
RUNNER_LINE = re.compile(
    r"@RC stage=runner ev=(?:run tape=[!-~]{1,12} key=(?P<run>%s)"
    r"|done result=(?:exit rc=(?:\d|[1-9]\d|1\d\d|2[0-4]\d|25[0-5])"
    r"|fault rc=-14|missing rc=-2|bad rc=-40|io rc=-5|eject rc=-41|error rc=-\d+) key=(?P<done>%s)"
    r"|done result=config rc=(?:0|-\d+) why=(?:open|read|size|parse|name|rand))" % (KEY, KEY))


def runner_lines(block):
    return [x for x in block.split("\n") if x.startswith("@RC stage=runner ")]


def run_end(lines):
    """The line that ends a run and its fields, by the rules of "When a run is over"."""
    key = None
    for line in lines:
        m = RUNNER_LINE.fullmatch(line)
        if m is None:
            if re.search(r"@RC .*\bev=(exception|panic)\b", line):
                return line, "fault"
            if re.search(r"@RC stage=pipos ev=ready\b", line):
                return line, "ready"
            continue
        if m["run"]:
            key = key or m["run"]
        elif m["done"]:
            if key is not None and m["done"] == key:
                return line, "done"
        elif key is None:
            return line, "config"
    return None, "running"


def test_runner_marker_examples_have_the_exact_form():
    lines = [x for text in (PACKAGE, CLI) for x in re.findall(r"@RC stage=runner [^\n\"]*", text)
             if "<" not in x and "`" not in x and "NAME" not in x and "..." not in x]
    assert len(lines) >= 10
    for line in lines:
        assert RUNNER_LINE.fullmatch(line), line
    # The config rows of the failure table give whole lines of the third form.
    table = section(PACKAGE, "What the runner accepts", "####")
    for tail in re.findall(r"`(rc=-?\d+ why=[a-z]+)`", table):
        assert RUNNER_LINE.fullmatch("@RC stage=runner ev=done result=config " + tail), tail


def test_runner_examples_end_by_the_key_of_the_run():
    runs = [b.rstrip("\n").split("\n") for b in blocks(PACKAGE, "") if "stage=runner" in b]
    assert len(runs) == 4
    ends = []
    for lines in runs:
        line, how = run_end(lines)
        ends.append(how)
        assert how in ("done", "config"), lines
        # The runner's line is followed by the kernel's two closing lines and nothing else.
        rest = lines[lines.index(line) + 1:]
        assert [re.sub(r"status=\d", "status=N", x) for x in rest] == \
            ["@RC stage=pipos ev=rom_exit status=N", "@RC stage=pipos ev=ready rom=1"]
        result = re.search(r"result=(\w+)", line)[1]
        assert rest[0].endswith("status=0" if result == "exit" else "status=1")
    assert ends.count("config") == 1
    # The run with a forged line: the tape's own done line is passed over.
    forged = next(r for r in runs if sum("ev=done" in x for x in r) == 2)
    line, how = run_end(forged)
    assert how == "done" and " rc=3 " in line and forged.index(line) > forged.index(
        next(x for x in forged if "ev=done" in x))
    # A tape that prints the forged lines and never ends leaves the run open.
    cut = forged[:forged.index(line)]
    assert run_end(cut) == (None, "running")
    assert run_end(cut + ["@RC stage=runner ev=done result=config rc=0 why=size"]) == (None, "running")
    assert run_end(cut + ["x@RC stage=pipos ev=panic msg=fake"])[1] == "fault"
    # In the JSON examples, a done line carries the key of the run line of the same block.
    for run in json_line_runs():
        texts = [o["text"] for _, o in run if o["kind"] == "serial"]
        keys = {m[1] for x in texts for m in [re.search(r"stage=runner .* key=(\w+)", x)] if m}
        assert len(keys) <= 1, texts
        for _, o in run:
            if o["kind"] == "result" and o["command"] == "run" and o["tape_result"]:
                m = RUNNER_LINE.fullmatch(o["line"])
                assert m and m["done"] and o["tape_rc"] == int(re.search(r"rc=(-?\d+)", o["line"])[1])


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


def test_backend_other_than_robco_is_an_unknown_key():
    codes = error_codes()
    errors = [o for run in json_line_runs() for _, o in run
              if o["kind"] == "error" and o["detail"].startswith("build.backend")]
    assert len(errors) == 1
    assert errors[0]["code"] == "manifest: unknown key" and codes[errors[0]["code"]] == 3
    assert re.fullmatch(r'build\.backend = "[^"\\]*"', errors[0]["detail"])
    line = next(b for b in blocks(PROJECT, "") if b.startswith("manifest: unknown key"))
    assert line == "manifest: unknown key: " + errors[0]["detail"] + "\n"


def test_boot_json_example_keeps_placeholders_out_of_names():
    testing = (SPEC / "TESTING.md").read_text(encoding="utf-8")
    boot = next(json.loads(b) for b in blocks(testing, "json") if '"boards"' in b)
    assert boot["format"] == 1
    lists = [boot["common"], boot["sd"]]
    for name, board in boot["boards"].items():
        # machine and qemu are names: a reader fills nothing in them.
        assert "{" not in board["machine"] and "{" not in board["qemu"], name
        assert board["default_mode"] in board["modes"], name
        lists += board["modes"].values()
    # Any argument list may hold a placeholder, and only the three exist.
    found = [m for args in lists for s in args for m in re.findall(r"\{[^}]*\}?", s)]
    assert set(found) == {"{stub}", "{image}", "{sd}"}
    assert all(type(s) is str for args in lists for s in args)
    section_text = section(testing, "boot.json")
    assert "Any argument list may hold a placeholder: `common`, `sd` and the list of each mode." in section_text
    assert "A `{` in either makes the file invalid" in section_text


def pbm_number(token):
    """The value of a width or height token, or None: 1 to 9 ASCII digits and nothing else."""
    if not 1 <= len(token) <= 9 or not all(0x30 <= b <= 0x39 for b in token):
        return None
    return int(token)


def test_pbm_number_forms_match_the_tables_of_the_image_spec():
    image = (SPEC / "IMAGE_FORMAT.md").read_text(encoding="utf-8")
    text = section(image, "Numbers in a PBM header", "####")
    assert "| Length | 1 to 9 digits. Leading zeros count toward the length. |" in text
    accepted = {b"32": 32, b"032": 32, b"0032": 32, b"000000032": 32, b"0": 0, b"00": 0,
                b"999999999": 999999999}
    for token, value in accepted.items():
        assert pbm_number(token) == value, token
    refused = [b"+32", b"-32", b"3_2", b"0x20", b"32.0", b"1e1", "\uff13\uff12".encode(),
               b"0000000032", b"32\x0b", b"\x0c32", b"", b" 32"]
    for token in refused:
        assert pbm_number(token) is None, token
    # Every token the refused table and the vector list spell out follows the rule.
    refused_table = text[text.index("These forms are refused"):text.index("State today")]
    for token in re.findall(r"`([^`]+)`", refused_table):
        if token not in ("io", "size", "0x0B", "0x0C", "0x30", "0x39", "32"):
            assert pbm_number(token.encode()) is None, token
    vectors = re.findall(r"Vector `(\w+)`: width `([^`]+)`[^.]*?, height `([^`]+)`[^.]*\. Expect `(\w+)`",
                         text)
    assert len(vectors) == 7
    for name, width, height, expect in vectors:
        if "0x0B" in text[text.index(name):text.index(name) + 80]:
            width += "\x0b"
        ok = pbm_number(width.encode()) == 32 and pbm_number(height.encode()) == 32
        assert expect == ("ok" if ok else "io"), name
        assert name.startswith("seal_holo_" if ok else "seal_err_"), name


def toml_string(value):
    """A basic string by the rules of "How the tool writes the lock"."""
    out = []
    for ch in value:
        if ch in '\\"':
            out.append("\\" + ch)
        elif ord(ch) < 0x20 or ord(ch) == 0x7F:
            out.append("\\u00%02x" % ord(ch))
        else:
            out.append(ch)
    return '"' + "".join(out) + '"'


def write_lock(index_location, packages, tools):
    """The bytes of robco.lock, by the layout of "How the tool writes the lock"."""
    lines = ["format = 1", "index = " + toml_string(index_location)]
    for package in sorted(packages, key=lambda p: p["name"].encode()):
        lines += ["", "[[package]]"]
        lines += [f"{key} = {toml_string(package[key])}" for key in ("name", "version", "sha256")]
    for tool in sorted(tools, key=lambda t: t["name"].encode()):
        lines += ["", "[[tool]]", f"name = {toml_string(tool['name'])}",
                  f"version = {toml_string(tool['version'])}", "", "[tool.archives]"]
        lines += [f"{host} = {toml_string(sha)}"
                  for host, sha in sorted(tool["archives"].items(), key=lambda kv: kv[0].encode())]
    return "".join(line + "\n" for line in lines)


def fits(entry, platform, abi):
    """The four fit rules of "Version and ABI rules", on an index entry alone."""
    if entry["name"] != platform or "abi" not in entry:
        return False
    have = [int(x) for x in entry["abi"].split(".")]
    want = [int(x) for x in abi.split(".")]
    known = set(entry["formats"]) <= KNOWN_FORMATS and set(entry["formats"].values()) <= {1}
    return have[0] == want[0] and have[1] >= want[1] and known


def resolve(index, platform, abi):
    """(packages, tools) for a lock, picked from the index and from nothing else."""
    fitting = [e for e in index["packages"] if fits(e, platform, abi)]
    if not fitting:
        return None
    best = max(fitting, key=lambda e: [int(x) for x in e["version"].split(".")])
    tools = []
    for named in best["tools"]:
        entry = next(t for t in index["tools"]
                     if (t["name"], t["version"]) == (named["name"], named["version"]))
        tools.append({"name": entry["name"], "version": entry["version"],
                      "archives": {h: a["sha256"] for h, a in entry["archives"].items()}})
    package = {"name": best["name"], "version": best["version"], "sha256": best["sha256"]}
    return [package], tools


def test_lock_example_is_what_a_resolution_of_the_index_example_writes():
    lock_text = next(b for b in blocks(PROJECT, "toml") if "[[package]]" in b)
    lock = tomllib.loads(lock_text)
    assert set(lock) == {"format", "index", "package", "tool"} and lock["format"] == 1
    # The example is in the writer's layout, byte for byte.
    assert write_lock(lock["index"], lock["package"], lock["tool"]) == lock_text
    # And it is what the index example gives for the first manifest.
    manifest = next(tomllib.loads(b) for b in blocks(PROJECT, "toml") if "[project]" in b)
    index = package_json('"packages"')
    packages, tools = resolve(index, manifest["target"]["platform"], manifest["target"]["abi"])
    assert write_lock(lock["index"], packages, tools) == lock_text
    # The forms of "Keys".
    for package in lock["package"]:
        assert set(package) == {"name", "version", "sha256"}
        assert re.fullmatch(r"[a-z][a-z0-9-]{0,31}", package["name"])
        assert re.fullmatch(VERSION, package["version"])
        assert re.fullmatch(r"[0-9a-f]{64}", package["sha256"])
    assert [t["name"] for t in lock["tool"]] == ["gcc", "qemu"]
    for tool in lock["tool"]:
        assert set(tool) == {"name", "version", "archives"} and tool["archives"]
        assert all(re.fullmatch(HOST, h) and re.fullmatch(r"[0-9a-f]{64}", s)
                   for h, s in tool["archives"].items())
    # A string that needs escapes, as a path on Windows does.
    assert toml_string('C:\\idx\\"a"\x1f.json') == '"C:\\\\idx\\\\\\"a\\"\\u001f.json"'
    assert tomllib.loads("index = " + toml_string('C:\\idx\\"a"\x1f.json'))["index"] == \
        'C:\\idx\\"a"\x1f.json'


def test_resolver_decides_from_the_index_alone():
    index = package_json('"packages"')
    # A newer package in a format this tool does not know is passed over, not picked.
    newer = dict(index_entry("pipos"), version="1.4.0", abi="1.4",
                 formats={"rimg": 1, "flags": 2, "boot": 1, "runner": 1})
    grown = dict(index, packages=index["packages"] + [newer])
    assert resolve(grown, "pipos", "1.3")[0][0]["version"] == "1.3.0"
    readable = dict(newer, formats=index_entry("pipos")["formats"])
    grown = dict(index, packages=index["packages"] + [readable])
    assert resolve(grown, "pipos", "1.3")[0][0]["version"] == "1.4.0"
    # No ABI is worked out from a version, and a templates package fits no project.
    assert resolve(index, "pipos", "1.4") is None
    assert resolve(index, "templates", "0.1") is None
    assert all("formats" in e for e in index["packages"])


def vkey(version):
    return [int(x) for x in version.split(".")]


def resolve_requires(entries, root_name, root_fits, root_text):
    """The passes of "Resolving requires". Returns {name: entry} or the error line."""
    root = ("root", root_name, None)
    reqs = {root}
    while True:
        picks, failed = {}, []
        for name in sorted({r[1] for r in reqs}):
            on = [r for r in reqs if r[1] == name]
            ok = [e for e in entries if e["name"] == name
                  and all(root_fits(e) if r[0] == "root" else fits_requires(e, r[2]) for r in on)]
            if ok:
                picks[name] = max(ok, key=lambda e: vkey(e["version"]))
            else:
                failed.append(name)
        if failed:
            name = failed[0]
            listed = sorted((r for r in reqs if r[1] == name and r[0] != "root"),
                            key=lambda r: (r[0][0].encode(), vkey(r[0][1])))
            parts = [f"{s[0]} {s[1]} abi {abi}" for s, _, abi in listed]
            if root[1] == name:
                parts.insert(0, root_text)
            line = f"sdk: no package: {name}: " + ", ".join(parts)
            # Versions that met every requirement but for rule 4.
            near = [e for e in entries if e["name"] == name and unknown_formats(e) and root[1] != name
                    and all(fits_requires(e, r[2], rule4=False) for r in on)]
            if near:
                e = max(near, key=lambda e: vkey(e["version"]))
                fmt = unknown_formats(e)[0]
                line += f"; {e['version']} needs a newer tool: {fmt} {e['formats'][fmt]}"
            return line
        added = {((e["name"], e["version"]), q["name"], q["abi"])
                 for e in picks.values() for q in e.get("requires", [])} - reqs
        if not added:
            break
        reqs |= added
    reached, todo = set(), [root_name]
    while todo:
        name = todo.pop()
        if name not in reached:
            reached.add(name)
            todo += [q["name"] for q in picks[name].get("requires", [])]
    return {n: picks[n] for n in sorted(reached)}


def versions(picks):
    return {n: e["version"] for n, e in picks.items()}


def unknown_formats(entry):
    """The formats of an entry that this tool does not know, sorted by name."""
    return sorted(f for f, v in entry["formats"].items() if f not in KNOWN_FORMATS or v != 1)


def fits_requires(entry, abi, rule4=True):
    """Rules 2, 3 and 4, with the abi of a requires entry. Rule 4 only when rule4 is set."""
    if "abi" not in entry:
        return False
    have, want = vkey(entry["abi"]), vkey(abi)
    return have[0] == want[0] and have[1] >= want[1] and not (rule4 and unknown_formats(entry))


def closed(packages, tools=None):
    """True when the index is closed, by "A closed index". Rule 4 plays no part."""
    for entry in packages:
        for q in entry.get("requires", []):
            if not any(e["name"] == q["name"] and fits_requires(e, q["abi"], rule4=False)
                       for e in packages):
                return False
        for named in entry.get("tools", []):
            if not any((t["name"], t["version"]) == (named["name"], named["version"]) and t["archives"]
                       for t in tools or []):
                return False
    return True


def spec_arrays():
    """The JSON arrays of the Examples section: the requires, open and limit indexes."""
    text = section(PACKAGE, "Examples", "##")
    return [json.loads(b) for b in blocks(text, "json") if b.lstrip().startswith("[")], text


def test_closed_index_and_the_limit_of_the_resolver():
    (requires, open_index, limit), text = spec_arrays()
    index = package_json('"packages"')
    # The writer accepts the other example indexes and refuses the one that is not closed.
    assert closed(index["packages"], index["tools"]) and closed(requires) and closed(limit)
    assert not closed(open_index)
    assert not closed(index["packages"], [t for t in index["tools"] if t["name"] != "qemu"])
    lines = [b.strip() for b in blocks(text, "text")]
    # The index that is not closed: the newest pipos is picked, and nothing goes back.
    wanted = next(x for x in lines if "no package: gfx" in x)
    abi13 = lambda e: fits(e, "pipos", "1.3")
    assert resolve_requires(open_index, "pipos", abi13, "robco.toml abi 1.3") == wanted
    assert wanted == "sdk: no package: gfx: pipos 1.4.0 abi 1.4"
    fixed = [e for e in open_index if e["version"] != "1.4.0"]
    assert closed(fixed)
    assert versions(resolve_requires(fixed, "pipos", abi13, "robco.toml abi 1.3")) == \
        {"gfx": "1.3.0", "pipos": "1.3.0"}
    # A closed index where rule 4 still strands the resolver, though app 1.0.0 would resolve.
    any_app = lambda e: not unknown_formats(e)
    wanted = next(x for x in lines if "needs a newer tool" in x)
    assert resolve_requires(limit, "app", any_app, "command line app") == wanted
    older = [e for e in limit if (e["name"], e["version"]) != ("app", "2.0.0")]
    assert versions(resolve_requires(older, "app", any_app, "command line app")) == \
        {"app": "1.0.0", "log": "1.4.0"}
    # Two majors of one name: closed, and the resolver still fails on it.
    majors = [
        {"name": "app", "version": "1.0.0", "abi": "1.0", "requires": [{"name": "log", "abi": "1.0"}],
         "formats": {}},
        {"name": "app", "version": "2.0.0", "abi": "2.0",
         "requires": [{"name": "log", "abi": "2.0"}, {"name": "ui", "abi": "1.0"}], "formats": {}},
        {"name": "ui", "version": "1.0.0", "abi": "1.0", "requires": [{"name": "log", "abi": "1.0"}],
         "formats": {}},
        {"name": "log", "version": "1.0.0", "abi": "1.0", "requires": [], "formats": {}},
        {"name": "log", "version": "2.0.0", "abi": "2.0", "requires": [], "formats": {}},
    ]
    assert closed(majors)
    assert resolve_requires(majors, "app", any_app, "command line app") == \
        "sdk: no package: log: app 2.0.0 abi 2.0, ui 1.0.0 abi 1.0"


def test_requires_example_resolves_by_name_over_every_requirement():
    text = section(PACKAGE, "Examples", "##")
    entries = next(json.loads(b) for b in blocks(text, "json") if b.lstrip().startswith("["))
    wanted = next(b for b in blocks(text, "text") if "no package: log" in b).strip()
    any_app = lambda e: set(e["formats"]) <= KNOWN_FORMATS
    picks = resolve_requires(entries, "app", any_app, "command line app")
    got = versions(picks)
    assert got == {"app": "1.0.0", "log": "1.5.2", "net": "1.0.0", "ui": "1.0.0"}
    assert list(got) == sorted(got, key=str.encode)
    # The order of the index and of a requires array does not count.
    flipped = [dict(e, requires=e["requires"][::-1]) for e in entries[::-1]]
    assert versions(resolve_requires(flipped, "app", any_app, "command line app")) == got
    # Each picked package meets every requirement the picked packages state.
    for e in picks.values():
        for q in e["requires"]:
            assert fits_requires(picks[q["name"]], q["abi"])
    # Without log 1.5.2, net and ui disagree, and the detail names both.
    short = [e for e in entries if (e["name"], e["version"]) != ("log", "1.5.2")]
    assert resolve_requires(short, "app", any_app, "command line app") == wanted
    assert resolve_requires(short[::-1], "app", any_app, "command line app") == wanted
    # A package the index does not hold at all.
    assert resolve_requires(entries, "nope", any_app, "command line nope") == \
        "sdk: no package: nope: command line nope"
    # The index example has one path: the passes pick what resolve() picks.
    index = package_json('"packages"')
    picks = resolve_requires(index["packages"], "pipos", lambda e: fits(e, "pipos", "1.3"),
                             "robco.toml abi 1.3")
    assert {n: e["version"] for n, e in picks.items()} == \
        {p["name"]: p["version"] for p in resolve(index, "pipos", "1.3")[0]}


def test_order_of_checks_names_a_known_code_for_every_step():
    codes = set(error_codes())
    text = section(CLI, "Order of checks")
    rows = [r for r in text.split("\n") if r.startswith("| ") and not r.startswith("| File")
            and not r.startswith("|---")]
    assert len(rows) == 7
    seen = set()
    for row in rows:
        cells = [c.strip() for c in row.strip("|").split("|")]
        assert len(cells) == 7, row
        for cell in cells[1:]:
            for code in re.findall(r"`((?:%s): [a-z][a-z ]*[a-z])`" % "|".join(CODE_PREFIXES), cell):
                assert code in codes, code
                seen.add(code)
        # Steps 2 and 3 always fail with a code, and step 3 with the format code of the file.
        assert "`" in cells[2] and "format`" in cells[3], row
    # Every code of a file that is refused when it is read has its place in the table.
    read_codes = {c for c in codes if c.split(":")[0] in ("manifest", "templates")
                  or c in ("lock: syntax", "lock: format", "lock: unknown key", "lock: entry",
                           "index: syntax", "index: format", "index: entry", "index: duplicate",
                           "test: syntax", "test: format", "test: unknown key", "project: none")}
    value_codes = {"manifest: name", "manifest: version", "manifest: title", "manifest: kind",
                   "manifest: subtype", "manifest: platform", "manifest: abi"}
    read_codes -= value_codes | {"templates: none"}    # no package installed: no file is read
    assert read_codes <= seen, sorted(read_codes - seen)


CODE_RE = r"`((?:%s)(?:: [a-z][a-z ]*[a-z])?)`" % "|".join(CODE_PREFIXES)
STEPS_HEADING = "The steps of update, build, run and test"


def anchors(text):
    """The anchors of the headings of one file, as GitHub makes them."""
    found, fence = {}, False
    for line in text.split("\n"):
        if line.startswith("```"):
            fence = not fence
        m = None if fence else re.match(r"#{1,6} +(.*?) *$", line)
        if m:
            anchor = re.sub(r"[^\w\- ]", "", m[1].lower()).replace(" ", "-")
            n = found.get(anchor, -1) + 1
            found[anchor] = n
            if n:
                found[f"{anchor}-{n}"] = 0
    return set(found)


def test_every_heading_a_spec_points_at_exists():
    files = {name: (SPEC / name).read_text(encoding="utf-8") for name in
             ("TESTING.md", "IMAGE_FORMAT.md", "PIPOS_ABI.md", "BOOT_PROTOCOL.md")}
    files.update({"STUDIO_CLI.md": CLI, "STUDIO_PROJECT.md": PROJECT, "SDK_PACKAGE.md": PACKAGE})
    known = {name: anchors(text) for name, text in files.items()}
    checked = 0
    for name in ("STUDIO_CLI.md", "STUDIO_PROJECT.md", "SDK_PACKAGE.md"):
        for target in re.findall(r"\]\(([^)\s]+)\)", files[name]):
            if target.startswith("http") or target.startswith("../"):
                continue
            path, _, anchor = target.partition("#")
            other = path[2:] if path.startswith("./") else (path or name)
            assert other in known, (name, target)
            if anchor:
                assert anchor in known[other], (name, target)
                checked += 1
    assert checked > 150
    # A section named in plain words, "step N of [Heading]", points at a list that long.
    for text in files.values():
        for n, heading in re.findall(r"steps? (\d+) of \[([^\]]+)\]", text):
            if heading == STEPS_HEADING:
                assert 1 <= int(n) <= 14


def steps_table():
    """The rows of the table of steps: (number, work, {command: cell}, fails-with cell)."""
    text = section(CLI, STEPS_HEADING, "##")
    rows = []
    for line in text.split("\n"):
        m = re.match(r"\| (\d+) \|", line)
        if m:
            cells = [c.strip() for c in line.strip("|").split("|")]
            assert len(cells) == 7, line
            rows.append((int(cells[0]), cells[1], dict(zip(("update", "build", "run", "test"),
                                                           cells[2:6])), cells[6]))
    return rows


def test_steps_table_gives_every_failure_its_exit_code():
    codes = error_codes()
    rows = steps_table()
    assert [n for n, *_ in rows] == list(range(1, 15))
    named = set()
    for n, work, cols, fails in rows:
        assert all(v for v in cols.values()), n
        groups = re.findall(r"((?:[^.]|\.(?! |$))*?), (\d+)(?: or (\d+))?\.?(?: |$)", fails)
        assert groups, (n, fails)
        for names, exit_code, other in groups:
            found = [c for c in re.findall(CODE_RE, names) if c in codes]
            families = re.findall(r"`([a-z]+):` code", names)    # a whole family, "A `test:` code"
            assert found or families, (n, names)
            for prefix in families:
                family = {c: e for c, e in codes.items() if c.startswith(prefix + ": ")}
                assert family, prefix
                wanted = set(family.values()) - ({1} if prefix == "test" else set())
                assert wanted == {int(exit_code)}, (n, prefix, wanted)
                named |= set(family) - {"test: failed"}
            for code in found:
                assert codes[code] == int(exit_code), (n, code)
                named.add(code)
            assert (other == "7") == ("test: failed" in found), (n, names)
    # Every code a project command can end with has its step. The others belong to
    # commands outside the table, or to no step.
    outside = {"new: name", "new: template", "new: exists", "templates: none", "templates: format",
               "templates: manifest", "templates: token", "firmware: hash", "internal"}
    assert set(codes) - named == outside, sorted(set(codes) - named - outside)
    # No tool is installed before the lock is read or resolved: finding 1.
    step = {work.split(":")[0].split(",")[0].split(" ")[0]: n for n, work, _, _ in rows}
    install_tools = next(n for n, work, _, _ in rows if work.startswith("Install the tools"))
    read_lock = next(n for n, work, _, _ in rows if work.startswith("Read `robco.lock`"))
    resolve = next(n for n, work, _, _ in rows if work.startswith("Resolve"))
    write_lock = next(n for n, work, _, _ in rows if work.startswith("Write `robco.lock`"))
    build = next(n for n, work, _, _ in rows if work.startswith("Build the tape"))
    start = next(n for n, work, _, _ in rows if work.startswith("Start QEMU"))
    assert read_lock < resolve < install_tools < write_lock < build < start, step
    tools = rows[install_tools - 1][2]
    assert "qemu" not in tools["build"] and tools["update"] == "No"
    assert "qemu" in tools["run"] and "qemu" in tools["test"]
    # run and test state no order of their own: they point at the table.
    for heading in ("robco run", "robco test", "robco build"):
        text = section(CLI, heading, "##")
        assert "(#the-steps-of-update-build-run-and-test)" in text, heading
        assert "first installs" not in text and "does everything `robco build` does" not in text
    assert "the-steps-of-update-build-run-and-test" in section(PROJECT, "robco update")


STEP_RANK = {"resolve": 0, "download": 1, "verify": 1, "install": 1, "compile": 2, "link": 3,
             "objcopy": 4, "seal": 5, "card": 6, "boot": 7}


def test_step_lines_of_the_examples_follow_the_steps_table():
    seen = set()
    for run in json_line_runs():
        command = run[-1][1]["command"]
        steps = [o for _, o in run if o["kind"] == "step"]
        ranks = [STEP_RANK[o["step"]] for o in steps]
        assert ranks == sorted(ranks), (command, [o["step"] for o in steps])
        items = [o["item"] for o in steps if STEP_RANK[o["step"]] == 1]
        order = []
        for item in items:
            name = item.split("@")[0]
            kind = 0 if "/" not in item else 1 if name == "gcc" else 2
            assert name == "qemu" or kind < 2, item
            if not order or order[-1] != kind:
                order.append(kind)
        assert order == sorted(order), (command, items)    # packages, then gcc, then qemu
        names = {i.split("@")[0] for i in items}
        if command == "build":
            assert "qemu" not in names
        if command == "update":
            assert not names & {"gcc", "qemu"}
        if "resolve" in [o["step"] for o in steps] and command == "run":
            # The first run of a project: the resolution comes before the qemu install.
            assert "qemu" in names
            seen.add("fresh run")
        for o in steps:
            if o["state"] == "skip":
                assert o["step"] == "install", o
            if o["step"] == "boot":
                assert o["item"] in BOARDS, o
        seen.add(command)
    assert seen >= {"sdk install", "update", "build", "run", "test", "fresh run"}


BOARDS = ("raspi0", "raspi1ap", "raspi2b", "raspi3b")


def table_values(text, key):
    """The backticked words of the row of `key` in a key table."""
    row = next(r for r in text.split("\n") if r.startswith(f"| `{key}` |"))
    return set(re.findall(r"`([a-z_]+)`", row.split("|", 2)[2]))


def test_every_kind_and_value_of_an_example_is_defined():
    kinds = set(re.findall(r"^\| `([a-z]+)` \|", section(CLI, "Kinds"), re.M)) - {"kind"}
    assert kinds == set(LINE_KEYS) | {"result"}
    # Each kind has a table of its keys, in the order the test holds.
    for kind, heading in (("step", "step"), ("diagnostic", "diagnostic"), ("serial", "serial"),
                          ("error", "error"), ("item", "robco sdk list")):
        text = section(CLI, heading, "###" if kind != "item" else "##")
        keys = re.findall(r"^\| `([a-z_]+)` \| (?:string|integer|boolean) \|", text, re.M)
        assert keys[:len(LINE_KEYS[kind])] == LINE_KEYS[kind], kind
    lines = section(CLI, "Lines of robco test")
    keys = [k for cell in re.findall(r"^\| ((?:`[a-z_]+`(?:, )?)+) \| (?:string|integer|boolean) \|",
                                     lines, re.M) for k in re.findall(r"`([a-z_]+)`", cell)]
    assert keys == (LINE_KEYS["scenario"] + LINE_KEYS["check"] + PIXEL_KEYS
                    + RESULT_ALWAYS_KEYS["test"])
    step_names = table_values(section(CLI, "step"), "step")
    assert step_names == STEPS == set(STEP_RANK)
    run_keys = section(CLI, "What ends a run")
    outcomes = {"matched", "ended", "timeout", "simulator", "guest", "not_run"}
    assert outcomes <= table_values(run_keys, "outcome")
    reasons = table_values(lines, "reason") - {"ok"}
    scenario_outcomes = table_values(lines, "outcome") - {"start", "done"}
    programs = table_values(section(CLI, "diagnostic"), "program")
    commands = {"sdk list", "sdk install", "new", "update", "build", "run", "test", ""}
    exits = {"matched": {0}, "ended": {0, 1}, "timeout": {6}, "simulator": {7}, "guest": {8}}
    used = {"kind": set(), "outcome": set(), "command": set()}
    for run in json_line_runs():
        for raw, o in run:
            assert o["kind"] in kinds, raw
            used["kind"].add(o["kind"])
            if o["kind"] == "step":
                assert o["step"] in step_names, raw
            elif o["kind"] == "diagnostic":
                assert o["program"] in programs, raw
            elif o["kind"] == "check":
                assert o["reason"] in reasons | {""} and (o["reason"] == "") == o["ok"], raw
                assert o["action"] in ACTIONS, raw
            elif o["kind"] == "scenario":
                assert o["outcome"] in scenario_outcomes | {""} and o["board"] in BOARDS, raw
                assert (o["outcome"] == "") == (o["state"] == "start"), raw
            elif o["kind"] == "result":
                assert o["command"] in commands, raw
                used["command"].add(o["command"])
                if o["command"] == "run":
                    assert o["outcome"] in outcomes, raw
                    used["outcome"].add(o["outcome"])
                    if o["outcome"] == "not_run":
                        assert o["exit"] not in (0, 1, 6, 8), raw
                        assert (o["line"], o["tape_result"], o["tape_rc"], o["ms"]) == ("", "", 0, 0)
                    else:
                        assert o["exit"] in exits[o["outcome"]], raw
                    if o["outcome"] in ("timeout", "simulator"):
                        assert o["line"] == "" and o["tape_result"] == "", raw
    assert used["kind"] == kinds, "every kind has an example"
    assert used["command"] == commands, "every command has an example"
    assert used["outcome"] >= {"matched", "ended", "timeout", "not_run"}


def test_what_ends_a_run_agrees_with_the_code_tables():
    codes = error_codes()
    text = section(CLI, "What ends a run")
    rules = [[c.strip() for c in r.strip("|").split("|")]
             for r in text.split("\n") if re.match(r"\| \d \|", r)]
    rules = [r for r in rules if len(r) == 5]    # the table of rules, not the table about tapes
    assert [int(r[0]) for r in rules] == list(range(1, 9))
    seen = set()
    for order, rule, outcome, exit_code, error in rules[1:]:
        outcome = outcome.strip("`")
        found = re.findall(CODE_RE, error)
        assert (found == []) == (exit_code == "0"), order
        for code in found:
            assert codes[code] == int(exit_code), (order, code)
            seen.add(code)
        assert {"matched": "0", "guest": "8"}.get(outcome, exit_code) == exit_code, order
    events = re.findall(r"^\| [^|`]+ \| `(timeout|simulator)` \| (\d) \| `([a-z: ]+)` \|$", text, re.M)
    assert len(events) == 3
    for outcome, exit_code, code in events:
        assert codes[code] == int(exit_code)
        seen.add(code)
    # Every code of a run is reached by a rule or an event, but the one of a QEMU that
    # never started.
    run_codes = {c for c in codes if c.split(":")[0] in ("guest", "sim") or
                 c in ("run: unmatched", "run: tape", "run: timeout")}
    assert run_codes - seen == {"sim: start"}
    # The outcome table of "Exit codes" gives the same exit codes.
    table = CLI[CLI.index("| Outcome of `robco run --headless` |"):CLI.index("`robco test` counts runs.")]
    rows = [[c.strip() for c in r.strip("|").split("|")] for r in table.split("\n")
            if r.startswith("| ") and "Outcome of" not in r]
    assert len(rows) == 7
    for row in rows:
        for cell in row[1:]:
            if cell:
                m = re.fullmatch(r"(\d)(?:, `([a-z: ]+)`)?", cell)
                assert m, cell
                assert m[2] is None or codes[m[2]] == int(m[1]), cell
    assert "The runner ROM could not start the tape" not in CLI


def test_every_code_is_used_outside_its_table():
    codes = error_codes()
    table = CLI[CLI.index("### Error codes"):CLI.index("### Order of checks")]
    rest = CLI.replace(table, "") + PROJECT + PACKAGE
    used = set(re.findall(CODE_RE, rest))
    # The manifest codes are named in their short form in the table of the project spec.
    short = set(re.findall(r"^\| `([a-z ]+)` \|", section(PROJECT, "Errors", "##"), re.M))
    used |= {f"manifest: {c}" for c in short}
    assert set(codes) - used == set(), sorted(set(codes) - used)
    # And every exit code but 0 has at least one code.
    assert set(codes.values()) == set(range(1, 11))
