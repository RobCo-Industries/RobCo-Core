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

Temp folders: a passing test's tmp_path goes when the test ends. A
test that fails or errors in setup, call or teardown keeps it. ROBCO_KEEP_TMP=1 keeps them all.
"""

import glob
import os
import re
import shutil
import subprocess
import sys

import pytest

from .boot import BOARDS
from .qemu import have_machine, sweep_stale
from .sdimage import have_mtools

DEFAULT_BOARDS = ["raspi0", "raspi2b"]


def pytest_sessionstart(session):
    """Stop the QEMU processes a dead earlier run left. Only its own.

    A courtesy: no error in it stops the session.
    """
    try:
        sweep_stale()
    except Exception as e:  # noqa: BLE001
        sys.stderr.write(f"robco harness: skipped the QEMU sweep: {e!r}\n")


KEEP_TMP_ENV = "ROBCO_KEEP_TMP"
RETENTION_INI = "tmp_path_retention_policy"


def _retention_set_by_user(config):
    """True when pytest.ini, setup.cfg, pyproject.toml or -o names the policy."""
    if RETENTION_INI in (config.inicfg or {}):
        return True
    overrides = getattr(config.known_args_namespace, "override_ini", None) or []
    return any(o.split("=", 1)[0].strip() == RETENTION_INI for o in overrides)


@pytest.hookimpl(trylast=True)
def pytest_configure(config):
    """Remove a passing test's tmp_path when the test ends.

    Card images and build copies fill the disk otherwise: pytest keeps
    every tmp_path of the last three runs. A failing test keeps its
    folder. ROBCO_KEEP_TMP=1 keeps every folder. A policy the project
    sets itself wins.

    pytest reads the policy into its factory in its own pytest_configure,
    which runs before this trylast hook, so the factory gets it here.
    """
    factory = getattr(config, "_tmp_path_factory", None)
    if factory is None or _retention_set_by_user(config):
        return
    keep = os.environ.get(KEEP_TMP_ENV, "") not in ("", "0")
    factory._retention_policy = "all" if keep else "failed"


try:
    from _pytest.tmpdir import tmppath_result_key as _PYTEST_RESULT_KEY
except ImportError:  # a pytest that renamed it: its own rule applies
    _PYTEST_RESULT_KEY = None

_TMP_PATH = pytest.StashKey()
_FAILED = pytest.StashKey()


def _deferring(item):
    """True when the harness, not pytest, removes this test's tmp_path."""
    factory = getattr(item.config, "_tmp_path_factory", None)
    return (_PYTEST_RESULT_KEY is not None
            and getattr(factory, "_retention_policy", None) == "failed"
            and _TMP_PATH in item.stash)


@pytest.hookimpl(hookwrapper=True)
def pytest_fixture_setup(fixturedef, request):
    """Note the test's tmp_path, also when a later fixture raises."""
    outcome = yield
    if fixturedef.argname == "tmp_path" and outcome.excinfo is None:
        request.node.stash[_TMP_PATH] = outcome.get_result()


@pytest.hookimpl(tryfirst=True)
def pytest_runtest_teardown(item):
    """Stop pytest's own removal, which runs in teardown.

    pytest's "failed" policy removes the folder unless the call phase
    failed. It misses a fixture that raises in setup and anything that
    fails in teardown. The harness decides after teardown instead.
    """
    if _deferring(item):
        item.stash.setdefault(_PYTEST_RESULT_KEY, {})["call"] = False


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item, call):
    outcome = yield
    rep = outcome.get_result()
    if rep.failed:
        item.stash[_FAILED] = True
    if rep.when == "teardown" and _deferring(item):
        path = item.stash[_TMP_PATH]
        if not item.stash.get(_FAILED, False):
            shutil.rmtree(str(path), ignore_errors=True)


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
