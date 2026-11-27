"""tools/release/cut_changelog.py on small made-up changelogs."""
import errno
import importlib.util
import os
import subprocess
import sys

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
TOOL = os.path.join(ROOT, "tools", "release", "cut_changelog.py")

# Plain bullets under `## Unreleased`.
PLAIN = """# Changelog

## Unreleased

- First change.
- Second change.
"""

# Keep a Changelog groups, with Added twice.
GROUPED = """# Changelog

Notes on the format.

## [Unreleased]

### Added
- Added one.

### Fixed
- Fixed one.

### Added
- Added two.
  A second line of added two.
"""

# An older section with a four-part version, and a footer after it.
OLD = """# Changelog

## [Unreleased]

### Changed
- Changed one.

### Added
- Added one.

### Changed
- Changed two.

## [7.1.0.8] - 2025-01-02

### Added
- Old entry.

---

## Version notes

Old footer.
"""


def tool(*args):
    return subprocess.run([sys.executable, "-I", TOOL, *args],
                          capture_output=True, text=True)


def write(tmp_path, text):
    path = tmp_path / "CHANGELOG.md"
    path.write_text(text)
    return path


def test_plain_heading_cut(tmp_path):
    path = write(tmp_path, PLAIN)
    r = tool("cut", str(path), "--version", "1.0.0", "--date", "2026-01-02")
    assert r.returncode == 0, r.stdout
    assert path.read_text() == """# Changelog

## Unreleased

## [1.0.0] - 2026-01-02

- First change.
- Second change.
"""


def test_grouped_cut_joins_repeats_in_first_order(tmp_path):
    path = write(tmp_path, GROUPED)
    r = tool("cut", str(path), "--version", "0.2.0", "--date", "2026-01-02")
    assert r.returncode == 0, r.stdout
    assert path.read_text() == """# Changelog

Notes on the format.

## [Unreleased]

## [0.2.0] - 2026-01-02

### Added

- Added one.
- Added two.
  A second line of added two.

### Fixed

- Fixed one.
"""


def test_old_four_part_sections_kept(tmp_path):
    path = write(tmp_path, OLD)
    r = tool("cut", str(path), "--version", "8.0.0", "--date", "2026-01-02")
    assert r.returncode == 0, r.stdout
    text = path.read_text()
    assert text.endswith(OLD[OLD.index("## [7.1.0.8]"):])
    new = text[text.index("## [8.0.0]"):text.index("## [7.1.0.8]")]
    assert new == ("## [8.0.0] - 2026-01-02\n\n### Changed\n\n- Changed one.\n"
                   "- Changed two.\n\n### Added\n\n- Added one.\n\n")
    assert tool("section", str(path), "--version", "8.0.0").stdout == new.rstrip() + "\n"


def test_every_entry_line_kept(tmp_path):
    for i, text in enumerate((PLAIN, GROUPED, OLD)):
        folder = tmp_path / str(i)
        folder.mkdir()
        path = write(folder, text)
        assert tool("cut", str(path), "--version", "9.0.0",
                    "--date", "2026-01-02").returncode == 0
        before = sorted(l for l in text.splitlines() if l.strip() and not l.startswith("#"))
        after = sorted(l for l in path.read_text().splitlines()
                       if l.strip() and not l.startswith("#"))
        assert before == after


@pytest.mark.parametrize("text", [PLAIN, GROUPED])
def test_cut_then_check_passes_and_second_cut_refused(tmp_path, text):
    path = write(tmp_path, text)
    assert tool("check", str(path), "--version", "1.0.0").returncode == 1
    assert tool("cut", str(path), "--version", "1.0.0", "--date", "2026-01-02").returncode == 0
    r = tool("check", str(path), "--version", "1.0.0")
    assert r.returncode == 0, r.stdout
    assert tool("check", str(path), "--version", "1.0.1").returncode == 1
    after = path.read_text()
    r = tool("cut", str(path), "--version", "1.0.0", "--date", "2026-01-03")
    assert r.returncode == 1
    assert "::error::" in r.stdout and "already has a section" in r.stdout
    assert path.read_text() == after


def test_empty_unreleased_refused(tmp_path):
    for text in ("# Changelog\n\n## [Unreleased]\n\n## [1.0.0] - 2026-01-01\n\n- a\n",
                 "# Changelog\n\n## Unreleased\n",
                 "# Changelog\n\n## [Unreleased]\n\n### Added\n\n### Fixed\n"):
        path = write(tmp_path, text)
        r = tool("cut", str(path), "--version", "1.1.0", "--date", "2026-01-02")
        assert r.returncode == 1
        assert "no entry" in r.stdout
        assert path.read_text() == text


def test_existing_version_refused(tmp_path):
    path = write(tmp_path, OLD.replace("7.1.0.8", "8.0.0"))
    r = tool("cut", str(path), "--version", "8.0.0", "--date", "2026-01-02")
    assert r.returncode == 1
    assert "already has a section" in r.stdout


def test_bad_arguments_refused(tmp_path):
    path = write(tmp_path, PLAIN)
    assert tool("cut", str(path), "--version", "1.0", "--date", "2026-01-02").returncode == 1
    assert tool("cut", str(path), "--version", "1.0.0.1", "--date", "2026-01-02").returncode == 1
    assert tool("cut", str(path), "--version", "1.0.0", "--date", "02/01/2026").returncode == 1
    # A leading zero is not a version, and the 30th of February is not a day.
    assert tool("cut", str(path), "--version", "01.0.0", "--date", "2026-01-02").returncode == 1
    assert tool("cut", str(path), "--version", "1.0.00", "--date", "2026-01-02").returncode == 1
    assert tool("cut", str(path), "--version", "1.0.0", "--date", "2026-02-30").returncode == 1
    assert tool("cut", str(path), "--version", "1.0.0", "--date", "2026-13-01").returncode == 1
    assert path.read_text() == PLAIN
    assert tool("check", str(tmp_path / "missing.md"), "--version", "1.0.0").returncode == 1
    nohead = write(tmp_path, "# Changelog\n\n## [1.0.0]\n")
    assert tool("check", str(nohead), "--version", "1.0.0").returncode == 1


@pytest.mark.parametrize("command", [
    ["cut", "--date", "2026-01-02"], ["check"], ["section"]])
def test_not_utf8_is_tool_error(tmp_path, command):
    path = tmp_path / "CHANGELOG.md"
    path.write_bytes(b"# Changelog\n\n## Unreleased\n\n- caf\xe9\n")
    r = tool(command[0], str(path), "--version", "1.0.0", *command[1:])
    assert r.returncode == 1
    assert r.stdout.startswith("::error::cut_changelog:")
    assert str(path) in r.stdout and "UTF-8" in r.stdout
    assert "Traceback" not in r.stderr
    assert path.read_bytes() == b"# Changelog\n\n## Unreleased\n\n- caf\xe9\n"


def test_section_missing_version(tmp_path):
    path = write(tmp_path, OLD)
    r = tool("section", str(path), "--version", "2.0.0")
    assert r.returncode == 1
    assert "has no section" in r.stdout


MIXED = """# Changelog

## Unreleased

### Changed
First paragraph.

Second paragraph.

### Fixed
- Fixed one.

### Changed
- Changed one.
  More on changed one.

### Changed
- Changed two.

### Changed
Closing paragraph.
"""


def test_join_keeps_paragraphs_and_joins_lists(tmp_path):
    path = write(tmp_path, MIXED)
    r = tool("cut", str(path), "--version", "1.0.0", "--date", "2026-01-02")
    assert r.returncode == 0, r.stdout
    text = path.read_text()
    assert text[text.index("### Changed"):] == """### Changed

First paragraph.

Second paragraph.

- Changed one.
  More on changed one.
- Changed two.

Closing paragraph.

### Fixed

- Fixed one.
"""
    changed = [l for l in MIXED.splitlines() if l.strip() and not l.startswith("#")]
    out = [l for l in text.splitlines() if l.strip() and not l.startswith("#")]
    assert [l for l in out if l != "- Fixed one."] == [
        l for l in changed if l != "- Fixed one."]


def load_tool():
    spec = importlib.util.spec_from_file_location("cut_changelog", TOOL)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def fail_fsync(fd):
    raise OSError(errno.EIO, "made-up write failure")


def test_failed_write_leaves_file_untouched(tmp_path, monkeypatch, capsys):
    path = write(tmp_path, GROUPED)
    os.chmod(path, 0o640)
    before = path.read_bytes()
    mod = load_tool()
    monkeypatch.setattr(mod.os, "fsync", fail_fsync)
    assert mod.run(["cut", str(path), "--version", "0.2.0", "--date", "2026-01-02"]) == 1
    assert "::error::" in capsys.readouterr().out
    assert path.read_bytes() == before
    assert os.listdir(tmp_path) == ["CHANGELOG.md"]


def test_cut_keeps_file_mode(tmp_path):
    path = write(tmp_path, PLAIN)
    os.chmod(path, 0o640)
    assert tool("cut", str(path), "--version", "1.0.0", "--date", "2026-01-02").returncode == 0
    assert os.stat(path).st_mode & 0o777 == 0o640
    assert os.listdir(tmp_path) == ["CHANGELOG.md"]
