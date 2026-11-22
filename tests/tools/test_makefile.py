"""Checks on the top-level Makefile that need no build."""

import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[2]


def dry_run(*args):
    out = subprocess.run(["make", "-n", "-C", str(ROOT), *args], check=True,
                         capture_output=True, text=True).stdout
    return [line for line in out.splitlines() if "pytest tests/target" in line]


def test_target_defaults_to_all_boards():
    assert dry_run("test-target")[0].endswith("--board all")


def test_target_honours_board():
    assert dry_run("test-target", "BOARD=raspi0")[0].endswith("--board raspi0")
