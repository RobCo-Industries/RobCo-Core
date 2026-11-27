"""tools/nightly/boot_card.py: the marker tables, the checksums and the snapshot.

The boot itself needs real stage images, so these tests feed made-up serial
text. The nightly run boots the real cards."""
import hashlib
import os
import sys

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "nightly"))
import boot_card  # noqa: E402

TERMINAL = """\
@RC stage=bios ev=boot midr=0x410fc075 soc=bcm2836 entry_mode=hyp version=0.1.0
@RC stage=bios ev=load path=/ROBCO/MFBOOT.SYS size=87956 crc=0xe9244c58 addr=0x00200000 name=MFBOOT
@RC stage=bios ev=jump entry=0x00200000
@RC stage=bootagent ev=boot midr=0x410fc075 entry_mode=svc version=0.1.0
@RC stage=bootagent ev=handoff trusted=1
@RC stage=bootagent ev=ready
@RC stage=bootagent ev=load path=/ROBCO/UOS.SYS size=275544 addr=0x00400000 name=UOS
@RC stage=bootagent ev=jump entry=0x00400000 name=UOS
@RC stage=uos ev=boot version=0.1.0 entry_mode=svc
@RC stage=uos ev=handoff trusted=1 prev=2 flags=0x00000004
@RC stage=uos ev=ready screen=login card=ok rtc=0 banner=51
""".splitlines()

PIPBOY = """\
@RC stage=pipos ev=boot midr=0x410fb767 soc=bcm2835 entry_mode=svc
PIP-OS(R) V7.1.0.8
@RC stage=pipos ev=load path=/PIPBOY/ROM1.ROM size=71672 addr=0x00400000 name=DEITRIX
@RC stage=pipos ev=rom rc=ok name=DEITRIX
@RC stage=pipos ev=jump entry=0x00400000
deitrix: load PROFILE.INI gauges=4
deitrix: ready
""".splitlines()

GOOD = {"terminal": TERMINAL, "pipboy": PIPBOY}


@pytest.mark.parametrize("chain", sorted(GOOD))
def test_a_whole_chain_passes(chain):
    assert boot_card.check(GOOD[chain], chain) == (len(boot_card.CHAINS[chain]), None)


def test_the_marker_form_of_the_rom_ready_line_passes():
    lines = PIPBOY[:-1] + ["@RC stage=deitrix ev=ready"]
    assert boot_card.check(lines, "pipboy") == (4, None)


def without(lines, text):
    return [line for line in lines if text not in line]


def swapped(lines, a, b):
    out = list(lines)
    i = next(n for n, line in enumerate(out) if a in line)
    j = next(n for n, line in enumerate(out) if b in line)
    out[i], out[j] = out[j], out[i]
    return out


# (chain, serial lines, steps seen, the problem names this or None)
BAD = {
    "terminal-no-bios-jump": ("terminal", without(TERMINAL, "bios ev=jump"), 2, None),
    "terminal-no-agent-handoff": ("terminal", without(TERMINAL, "bootagent ev=handoff"), 4, None),
    "terminal-untrusted-uos": ("terminal", [line.replace("trusted=1 prev", "trusted=0 prev")
                                            for line in TERMINAL], 8, None),
    "terminal-no-login-screen": ("terminal", [line.replace("screen=login", "screen=first")
                                              for line in TERMINAL], 9, None),
    "terminal-stops-at-the-agent": ("terminal", TERMINAL[:6], 5, None),
    "terminal-uos-before-the-agent-jump": ("terminal",
                                           swapped(TERMINAL, "bootagent ev=jump", "uos ev=boot"),
                                           7, None),
    "terminal-agent-loads-backup": ("terminal", [line.replace("path=/ROBCO/MFBOOT.SYS",
                                                              "path=/ROBCO/MFBOOT.BAK")
                                                 for line in TERMINAL], 1, None),
    "terminal-load-error": ("terminal", TERMINAL[:6] + [
        "@RC stage=bootagent ev=error code=nofile path=/ROBCO/UOS.SYS"], 5, "ev=error"),
    "terminal-panic-after-ready": ("terminal", TERMINAL + ["@RC stage=uos ev=panic msg=x"],
                                   10, "ev=panic"),
    "pipboy-no-load": ("pipboy", without(PIPBOY, "ev=load"), 1, None),
    "pipboy-no-jump": ("pipboy", without(PIPBOY, "ev=jump"), 2, None),
    "pipboy-no-rom-ready": ("pipboy", without(PIPBOY, "deitrix: ready"), 3, None),
    "pipboy-ready-before-jump": ("pipboy", swapped(PIPBOY, "ev=jump", "deitrix: ready"), 3, None),
    "pipboy-ready-inside-a-line": ("pipboy", PIPBOY[:-1] + ["x deitrix: ready later"], 3, None),
    "pipboy-rom-killed": ("pipboy", PIPBOY[:5] + [
        "@RC stage=pipos ev=rom_kill reason=undef pc=0x00400010"], 3, "ev=rom_kill"),
}


@pytest.mark.parametrize("case", sorted(BAD))
def test_a_broken_chain_fails(case):
    chain, lines, steps, says = BAD[case]
    seen, problem = boot_card.check(lines, chain)
    assert seen == steps
    assert seen < len(boot_card.CHAINS[chain]) or problem
    if says:
        assert problem and says in problem, problem
    else:
        assert problem is None


@pytest.fixture
def kept(tmp_path):
    card = tmp_path / "robco-terminal.img"
    card.write_bytes(b"card" * 1000)
    first = tmp_path / "retros.img"
    first.write_bytes(b"bios")
    sums = tmp_path / "SHA256SUMS"
    sums.write_text(f"{hashlib.sha256(card.read_bytes()).hexdigest()}  robco-terminal.img\n")
    return card, first, sums


def run(kept, lines=TERMINAL, during=None):
    card, first, sums = kept
    calls = []

    def fake(first_img, board, sd, timeout, chain):
        calls.append((first_img, board, sd))
        assert open(sd, "rb").read() == card.read_bytes() and sd != str(card)
        if during:
            during(sd)
        return lines

    old = boot_card.boot_and_read
    boot_card.boot_and_read = fake
    try:
        rc = boot_card.run(["--chain", "terminal", "--board", "raspi2b", "--first", str(first),
                            "--card", str(card), "--sums", str(sums), "--timeout", "5"])
    finally:
        boot_card.boot_and_read = old
    return rc, calls


def test_the_copy_boots_and_the_card_still_matches(kept, capsys):
    rc, calls = run(kept)
    assert rc == 0, capsys.readouterr().out
    assert len(calls) == 1 and calls[0][1] == "raspi2b"
    assert not os.path.exists(calls[0][2])


def test_a_card_that_does_not_match_its_sums_is_not_booted(kept):
    kept[0].write_bytes(b"other")
    with pytest.raises(SystemExit, match="does not match SHA256SUMS"):
        run(kept)


def test_a_card_missing_from_the_sums_is_not_booted(kept):
    kept[2].write_text("")
    with pytest.raises(SystemExit, match="does not list"):
        run(kept)


def test_a_write_to_the_copy_fails(kept, capsys):
    def write(sd):
        with open(sd, "r+b") as f:
            f.write(b"X")
    rc, _ = run(kept, during=write)
    assert rc == 1
    assert "snapshot did not hold" in capsys.readouterr().out


def test_a_change_to_the_kept_card_fails(kept, capsys):
    rc, _ = run(kept, during=lambda sd: kept[0].write_bytes(b"changed"))
    assert rc == 1
    assert "changed during the boot" in capsys.readouterr().out


def test_a_broken_chain_fails_the_run(kept, capsys):
    rc, _ = run(kept, lines=TERMINAL[:6])
    out = capsys.readouterr().out
    assert rc == 1
    assert "MISS MFBootAgent loads UOS" in out and "::error::boot_card:" in out
