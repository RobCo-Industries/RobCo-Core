"""tools/nightly/junit_gate.py on made-up JUnit files and on a real pytest run."""
import os
import subprocess
import sys

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
GATE = os.path.join(ROOT, "tools", "nightly", "junit_gate.py")


def case(name, inner=""):
    return f'<testcase classname="tests.test_chain" name="{name}" time="1.0">{inner}</testcase>'


PASS = case("test_whole_chain[raspi0]")
PASS2 = case("test_whole_chain_keeps_bios_text[raspi0]")
FAIL = case("test_whole_chain[raspi1ap]", '<failure message="AssertionError: no jump">tb</failure>')
ERROR = case("test_whole_chain[raspi2b]", '<error message="failed on setup">tb</error>')
SKIP = case("test_whole_chain[raspi3b]",
            '<skipped type="pytest.skip" message="RETROS_IMG is not set">skip</skipped>')
TIMING = case("test_whole_chain_keeps_bios_text[raspi3b]",
              '<skipped type="pytest.skip" message="host timing: tick starved">skip</skipped>')
TIMING2 = case("test_whole_chain_other[raspi3b]",
               '<skipped type="pytest.skip" message="host timing: tick starved">skip</skipped>')


def junit(*cases):
    return ('<?xml version="1.0" encoding="utf-8"?><testsuites><testsuite name="pytest">'
            + "".join(cases) + "</testsuite></testsuites>")


def gate(tmp_path, xml=None, extra=()):
    path = tmp_path / "junit.xml"
    if xml is not None:
        path.write_text(xml)
    summary = tmp_path / "summary.md"
    proc = subprocess.run([sys.executable, "-I", GATE, "--job", "agent-chain raspi0",
                           str(path), *extra, "--summary", str(summary)],
                          capture_output=True, text=True)
    return proc, summary.read_text() if summary.exists() else ""


def test_all_passed_is_green(tmp_path):
    proc, summary = gate(tmp_path, junit(PASS, PASS2))
    assert proc.returncode == 0, proc.stdout
    assert summary.startswith("### agent-chain raspi0: green")
    assert "2 passed, 0 failed, 0 errored, 0 skipped" in summary
    assert "- `tests.test_chain::test_whole_chain[raspi0]`: passed" in summary
    assert "::error" not in proc.stdout


@pytest.mark.parametrize("bad, reason", [
    (FAIL, "test_whole_chain[raspi1ap] failed"),
    (ERROR, "test_whole_chain[raspi2b] errored"),
    (SKIP, "test_whole_chain[raspi3b] skipped: RETROS_IMG is not set"),
])
def test_one_bad_test_is_red(tmp_path, bad, reason):
    proc, summary = gate(tmp_path, junit(PASS, bad))
    assert proc.returncode == 1
    assert "red" in summary.splitlines()[0]
    assert reason in summary
    assert f"::error title=agent-chain raspi0::tests.test_chain::{reason}" in proc.stdout


def test_all_skipped_is_red(tmp_path):
    proc, summary = gate(tmp_path, junit(SKIP, SKIP.replace("raspi3b", "raspi0")))
    assert proc.returncode == 1
    assert "no test passed" in summary


def test_one_host_timing_skip_passes(tmp_path):
    proc, summary = gate(tmp_path, junit(PASS, TIMING))
    assert proc.returncode == 0, proc.stdout
    assert "skipped, host timing: tick starved" in summary


def test_two_host_timing_skips_are_red(tmp_path):
    proc, summary = gate(tmp_path, junit(PASS, TIMING, TIMING2))
    assert proc.returncode == 1
    assert "test_whole_chain_other[raspi3b] skipped: host timing" in summary
    assert "keeps_bios_text[raspi3b] skipped" not in summary.split("- `")[0]


def test_only_a_host_timing_skip_is_red(tmp_path):
    proc, summary = gate(tmp_path, junit(TIMING))
    assert proc.returncode == 1
    assert "no test passed" in summary


def test_no_test_is_red(tmp_path):
    proc, summary = gate(tmp_path, junit())
    assert proc.returncode == 1
    assert "the file holds no test" in summary


@pytest.mark.parametrize("xml, reason", [
    (None, "is missing"),
    ("", "is not valid XML"),
    ("<html/>", "is not a JUnit file"),
])
def test_unreadable_file_is_red(tmp_path, xml, reason):
    proc, summary = gate(tmp_path, xml)
    assert proc.returncode == 1
    assert reason in summary and reason in proc.stdout


def test_a_rerun_file_is_refused(tmp_path):
    proc, _ = gate(tmp_path, junit(PASS), extra=(str(tmp_path / "rerun.xml"),))
    assert proc.returncode == 2
    assert "rerun" in proc.stderr


def test_a_real_pytest_run(tmp_path):
    """pytest's own JUnit output: a pass, a failure, an error in setup, a skip."""
    src = tmp_path / "src"
    src.mkdir()
    (src / "pytest.ini").write_text("[pytest]\n")
    (src / "test_x.py").write_text(
        "import pytest\n"
        "@pytest.fixture\n"
        "def broken():\n    raise RuntimeError('no image')\n"
        "def test_whole_chain_ok():\n    pass\n"
        "def test_whole_chain_fails():\n    assert 1 == 2\n"
        "def test_whole_chain_errors(broken):\n    pass\n"
        "def test_whole_chain_timing():\n    pytest.skip('host timing: tick starved')\n"
        "def test_whole_chain_skips():\n    pytest.skip('RETROS_IMG is not set')\n")

    def pytest_run(name, select):
        xml = tmp_path / f"{name}.xml"
        subprocess.run([sys.executable, "-m", "pytest", "-q", "-p", "no:cacheprovider",
                        "--rootdir", str(src), "-c", str(src / "pytest.ini"), str(src),
                        "-k", select, f"--junitxml={xml}"],
                       capture_output=True, text=True, cwd=src)
        return xml.read_text()

    proc, summary = gate(tmp_path, pytest_run("good", "ok or timing"))
    assert proc.returncode == 0, summary
    assert "1 passed, 0 failed, 0 errored, 1 skipped" in summary
    proc, summary = gate(tmp_path, pytest_run("bad", "whole_chain"))
    assert proc.returncode == 1
    assert "1 passed, 1 failed, 1 errored, 2 skipped" in summary
    for reason in ("test_whole_chain_fails failed", "test_whole_chain_errors errored",
                   "test_whole_chain_skips skipped: RETROS_IMG is not set"):
        assert reason in summary
    assert "test_whole_chain_timing skipped" not in summary.split("- `")[0]
