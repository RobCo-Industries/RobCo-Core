"""Boots the sample image through harness.plugin's fixtures."""

import os

from harness.boot import boot

PROJECT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def test_ready(board, robco_build):
    image = robco_build(PROJECT)
    assert image == os.path.join(PROJECT, "build", "sample.img")
    with boot(image, board) as vm:
        vm.wait_for("ev=ready", timeout=30, line=True)
        ready = [m for m in vm.markers() if m.get("ev") == "ready"]
        assert ready[0]["stage"] == "sample" and ready[0]["text"].startswith("hello")
