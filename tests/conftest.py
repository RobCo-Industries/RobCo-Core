"""Pytest setup for RobCo-Core's own tests.

harness.plugin gives --board, the board, need_mtools and robco_build
fixtures; projects enable the same plugin. This file adds the spikes and
examples of this repository on top.
"""

import os
import subprocess
import sys

import pytest

TESTS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TESTS)
sys.path.insert(0, TESTS)

pytest_plugins = ["harness.plugin"]

SPIKES = {"s1": "s1-uart-hello", "s2": "s2-sd-read", "s3": "s3-fb-mmu"}


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


@pytest.fixture(scope="session")
def example_image(robco_build):
    """example_image("hello") builds examples/hello once per session and returns its .img path.

    variant names a build dir under the example, "build/<variant>".
    extra_cflags goes to EXTRA_CFLAGS for that variant.
    """
    def get(name, variant=None, extra_cflags=None):
        flags = [f"EXTRA_CFLAGS={extra_cflags}"] if extra_cflags else []
        return robco_build(os.path.join(ROOT, "examples", name), variant=variant, flags=flags)

    return get
