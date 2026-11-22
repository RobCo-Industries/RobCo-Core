"""Checks for tools/font2c.py. Host only, no QEMU."""

import os
import re
import subprocess
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOL = os.path.join(ROOT, "tools", "font2c.py")
SPLEEN = "third_party/spleen/spleen-8x16.bdf"
FIXEDSYS = "third_party/fixedsys-excelsior/FSEX302.ttf"
GLYPH_COUNT = 95 + 23


def run(*args, ok=True):
    r = subprocess.run([sys.executable, TOOL, *args], cwd=ROOT, capture_output=True, text=True)
    if ok:
        assert r.returncode == 0, r.stderr
    else:
        assert r.returncode != 0, r.stdout
    return r


def bdf(path, glyphs, bbx=(8, 16, 0, -4)):
    """Writes a BDF with the given {code point: (bbx, rows)} glyphs."""
    lines = ["STARTFONT 2.1", "FONT test", "SIZE 16 72 72", "FONTBOUNDINGBOX 8 16 0 -4",
             "STARTPROPERTIES 2", "FONT_ASCENT 12", "FONT_DESCENT 4", "ENDPROPERTIES",
             f"CHARS {len(glyphs)}"]
    for cp, (gbbx, rows) in glyphs.items():
        w = gbbx[0]
        lines += [f"STARTCHAR U{cp:04X}", f"ENCODING {cp}", "SWIDTH 500 0", "DWIDTH 8 0",
                  "BBX " + " ".join(str(v) for v in gbbx), "BITMAP"]
        lines += [f"{r:0{2 * ((w + 7) // 8)}X}" for r in rows]
        lines.append("ENDCHAR")
    lines.append("ENDFONT")
    path.write_text("\n".join(lines) + "\n")
    return path


def full_ascii(skip=()):
    return {cp: ((8, 16, 0, -4), [cp & 0xFF] * 16) for cp in range(0x20, 0x7F) if cp not in skip}


@pytest.mark.parametrize("src, name, select, committed", [
    (SPLEEN, "spleen", "ROBCO_FONT_SPLEEN", "src/gfx/font8x16_spleen.c"),
    (FIXEDSYS, "fixedsys-excelsior", "ROBCO_FONT_FIXEDSYS", "src/gfx/font8x16_fixedsys.c"),
])
def test_table_regenerates_identically(tmp_path, src, name, select, committed):
    if src.endswith(".ttf"):
        pytest.importorskip("PIL")
    out = tmp_path / "font8x16.c"
    run(src, "--name", name, "--select", select, "-o", str(out))
    with open(os.path.join(ROOT, committed)) as f:
        want = f.read()
    # The generator line names the output, so compare with the committed path in it.
    got = out.read_text().replace(str(out), committed)
    assert got == want


def test_select_wraps_table(tmp_path):
    out = tmp_path / "f.c"
    run(SPLEEN, "--name", "t", "--select", "ROBCO_FONT_SPLEEN", "-o", str(out))
    text = out.read_text()
    assert "#if ROBCO_FONT == ROBCO_FONT_SPLEEN\n" in text
    assert text.endswith("#endif /* ROBCO_FONT == ROBCO_FONT_SPLEEN */\n")
    run(SPLEEN, "--name", "t", "--select", "SPLEEN", "-o", str(out), ok=False)


def test_header_and_name(tmp_path):
    out = tmp_path / "f.c"
    r = run(SPLEEN, "--name", "my-font", "-o", str(out))
    text = out.read_text()
    assert " * Source: spleen-8x16.bdf\n" in text
    assert re.search(r" \* SHA-256: [0-9a-f]{64}\n", text)
    assert " * This table inherits the licence of its source font.\n" in text
    assert 'return "my-font";' in text
    assert f"{GLYPH_COUNT} of {GLYPH_COUNT} glyphs" in r.stderr


def test_bad_name(tmp_path):
    r = run(SPLEEN, "--name", "bad name", "-o", str(tmp_path / "f.c"), ok=False)
    assert "--name" in r.stderr


def test_wide_glyph_fails(tmp_path):
    glyphs = full_ascii()
    glyphs[0x41] = ((9, 16, 0, -4), [0x1FF] * 16)
    r = run(str(bdf(tmp_path / "wide.bdf", glyphs)), "--name", "t", "-o", str(tmp_path / "f.c"), ok=False)
    assert "U+0041 'A'" in r.stderr and "does not fit" in r.stderr


def test_tall_glyph_fails(tmp_path):
    glyphs = full_ascii()
    glyphs[0x42] = ((8, 18, 0, -5), [0xFF] * 18)
    r = run(str(bdf(tmp_path / "tall.bdf", glyphs)), "--name", "t", "-o", str(tmp_path / "f.c"), ok=False)
    assert "U+0042 'B'" in r.stderr and "does not fit" in r.stderr


def test_missing_glyph_falls_back(tmp_path):
    out = tmp_path / "f.c"
    r = run(str(bdf(tmp_path / "gap.bdf", full_ascii(skip={0x5A}))), "--name", "t", "-o", str(out))
    assert "U+005A 'Z'" in r.stderr
    assert f"{95 - 1} of {GLYPH_COUNT} glyphs" in r.stderr
    rep = "{0x00, 0x00, 0x7e, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x7e, 0x00, 0x00, 0x00}, /* U+005A */"
    assert rep in out.read_text()
    assert "font_extra[0]" in out.read_text()


def test_unknown_type(tmp_path):
    src = tmp_path / "font.pcf"
    src.write_bytes(b"")
    r = run(str(src), "--name", "t", "-o", str(tmp_path / "f.c"), ok=False)
    assert "unknown font type" in r.stderr


def test_ttf(tmp_path):
    pytest.importorskip("PIL")
    out = tmp_path / "f.c"
    r = run(FIXEDSYS, "--name", "ttf-test", "-o", str(out))
    text = out.read_text()
    assert 'return "ttf-test";' in text
    assert re.search(r"Generator: tools/font2c.py .* --size 16 -o ", text)
    assert len(re.findall(r"/\* U\+00[2-7][0-9A-F] \*/", text)) == 95
    assert "#if" not in text
    assert f"{GLYPH_COUNT} of {GLYPH_COUNT} glyphs" in r.stderr
