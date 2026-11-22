"""Shared pytest options and fixtures for target tests.

--board NAME   run board-parametrised tests on NAME (repeatable).
               Default: raspi0 and raspi2b. "all" adds raspi1ap and raspi3b.
"""

import os
import subprocess
import sys

import pytest

TESTS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TESTS)
sys.path.insert(0, TESTS)

from harness.boot import BOARDS  # noqa: E402
from harness.qemu import have_machine  # noqa: E402
from harness.sdimage import have_mtools  # noqa: E402

DEFAULT_BOARDS = ["raspi0", "raspi2b"]
SPIKES = {"s1": "s1-uart-hello", "s2": "s2-sd-read", "s3": "s3-fb-mmu"}


def pytest_addoption(parser):
    parser.addoption("--board", action="append", default=[],
                     help="board to test on (repeatable, or 'all')")


def selected_boards(config):
    names = config.getoption("--board") or DEFAULT_BOARDS
    if "all" in names:
        return list(BOARDS)
    unknown = [n for n in names if n not in BOARDS]
    if unknown:
        raise pytest.UsageError(f"unknown --board {unknown}, expected {list(BOARDS)} or all")
    return list(dict.fromkeys(names))


def pytest_generate_tests(metafunc):
    if "board" in metafunc.fixturenames:
        metafunc.parametrize("board", selected_boards(metafunc.config), indirect=True)


@pytest.fixture
def board(request):
    name = request.param
    if not have_machine(BOARDS[name].binary, name):
        pytest.skip(f"{BOARDS[name].binary} -M {name} not available")
    return name


@pytest.fixture(scope="session")
def spike_image():
    """spike_image("s1") builds a spike once per session and returns its .img path."""
    built = {}

    def get(name):
        if name not in built:
            d = os.path.join(ROOT, "spikes", SPIKES[name])
            subprocess.run(["make", "-s", "-C", d], check=True, stdout=subprocess.DEVNULL)
            built[name] = os.path.join(d, "build", f"{name}.img")
        return built[name]

    return get


@pytest.fixture
def need_mtools():
    if not have_mtools():
        pytest.skip("mtools (mformat, mcopy, mmd) not installed")
