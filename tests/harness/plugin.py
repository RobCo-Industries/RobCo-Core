"""pytest plugin for target tests: the --board option and build fixtures.

A project enables it in its tests/conftest.py:

    import os, sys
    sys.path.insert(0, os.path.join(os.path.dirname(__file__), "../external/robco-core/tests"))
    pytest_plugins = ["harness.plugin"]

--board NAME   run board-parametrised tests on NAME (repeatable).
               Default: raspi0 and raspi2b, or the robco_boards ini value.
               "all" selects every board.

Fixtures:
  board         the board name. Skips when the installed QEMU lacks it.
  need_mtools   skips the test without mtools.
  robco_build   robco_build(directory, target="all", variant=None, flags=(),
                image=None) runs make once per (directory, target, variant,
                flags) in the session and returns the image path.
"""

import glob
import os
import re
import subprocess

import pytest

from .boot import BOARDS
from .qemu import have_machine
from .sdimage import have_mtools

DEFAULT_BOARDS = ["raspi0", "raspi2b"]


def pytest_addoption(parser):
    parser.addoption("--board", action="append", default=[],
                     help="board to test on (repeatable, or 'all')")
    parser.addini("robco_boards", "boards to test on without --board", type="args",
                  default=list(DEFAULT_BOARDS))


def selected_boards(config):
    names = config.getoption("--board") or config.getini("robco_boards") or DEFAULT_BOARDS
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


@pytest.fixture
def need_mtools():
    if not have_mtools():
        pytest.skip("mtools (mformat, mcopy, mmd) not installed")


def image_path(directory, build_dir):
    """The .img a Makefile with `IMAGE := name` writes into build_dir."""
    makefile = os.path.join(directory, "Makefile")
    if os.path.exists(makefile):
        with open(makefile) as f:
            m = re.search(r"^IMAGE\s*[:?]?=\s*(\S+)", f.read(), re.M)
        if m:
            return os.path.join(build_dir, m.group(1) + ".img")
    imgs = glob.glob(os.path.join(build_dir, "*.img"))
    if len(imgs) != 1:
        raise RuntimeError(f"expected one .img in {build_dir}, found {imgs}")
    return imgs[0]


@pytest.fixture(scope="session")
def robco_build():
    """robco_build(directory, ...) builds once per session and returns the image path.

    Runs `make -s -C directory CI=1 BUILD=<build> <flags> <target>`.
    variant builds into build/<variant> instead of build. flags are
    "NAME=value" strings. image names the result relative to the build
    directory; without it the path comes from IMAGE in the Makefile.
    """
    built = {}

    def get(directory, target="all", variant=None, flags=(), image=None):
        directory = os.path.abspath(directory)
        key = (directory, target, variant, tuple(flags), image)
        if key not in built:
            build = f"build/{variant}" if variant else "build"
            cmd = ["make", "-s", "-C", directory, "CI=1", f"BUILD={build}", *flags, target]
            subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
            build_dir = os.path.join(directory, build)
            built[key] = (os.path.join(build_dir, image) if image
                          else image_path(directory, build_dir))
        return built[key]

    return get
