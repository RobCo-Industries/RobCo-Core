"""Shared pytest options and fixtures for target tests.

--board NAME   run board-parametrised tests on NAME (repeatable).
               Default: raspi0 and raspi2b. "all" adds raspi1ap and raspi3b.
"""

import glob
import os
import re
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


def _image_name(example_dir, build_dir):
    with open(os.path.join(example_dir, "Makefile")) as f:
        m = re.search(r"^IMAGE\s*[:?]?=\s*(\S+)", f.read(), re.M)
    if m:
        return os.path.join(build_dir, m.group(1) + ".img")
    imgs = glob.glob(os.path.join(build_dir, "*.img"))
    if len(imgs) != 1:
        raise RuntimeError(f"expected one .img in {build_dir}, found {imgs}")
    return imgs[0]


@pytest.fixture(scope="session")
def example_image():
    """example_image("hello") builds examples/hello once per session and returns its .img path.

    variant names a build dir under the example, "build/<variant>".
    extra_cflags goes to EXTRA_CFLAGS for that variant.
    """
    built = {}

    def get(name, variant=None, extra_cflags=None):
        key = (name, variant)
        if key not in built:
            d = os.path.join(ROOT, "examples", name)
            build = f"build/{variant}" if variant else "build"
            cmd = ["make", "-s", "-C", d, "CI=1", f"BUILD={build}"]
            if extra_cflags:
                cmd.append(f"EXTRA_CFLAGS={extra_cflags}")
            subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
            built[key] = _image_name(d, os.path.join(d, build))
        return built[key]

    return get


@pytest.fixture
def need_mtools():
    if not have_mtools():
        pytest.skip("mtools (mformat, mcopy, mmd) not installed")
