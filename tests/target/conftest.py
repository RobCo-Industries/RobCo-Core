"""Fails a target session that skips too many tests for host timing.

A host timing skip is one made through harness.hosttiming.skip(). The
session allows one per selected board, or --timing-skips-max=N.
"""

import pytest

from harness.hosttiming import is_timing_skip
from harness.plugin import selected_boards


def pytest_addoption(parser):
    parser.addoption("--timing-skips-max", type=int, default=None,
                     help="host timing skips allowed in the session (default: one per board)")


def pytest_configure(config):
    config.pluginmanager.register(TimingSkips(config), "robco-timing-skips")


class TimingSkips:
    def __init__(self, config):
        self.config = config
        self.skips = []

    def limit(self):
        n = self.config.getoption("--timing-skips-max")
        return len(selected_boards(self.config)) if n is None else n

    def over(self):
        return len(self.skips) > self.limit()

    def pytest_runtest_logreport(self, report):
        if report.skipped and isinstance(report.longrepr, tuple) and is_timing_skip(report.longrepr[2]):
            self.skips.append((report.nodeid, report.longrepr[2]))

    def pytest_sessionfinish(self, session):
        if self.over() and session.exitstatus == pytest.ExitCode.OK:
            session.exitstatus = pytest.ExitCode.TESTS_FAILED

    def pytest_terminal_summary(self, terminalreporter):
        if not self.skips:
            return
        over = self.over()
        terminalreporter.section("host timing skips", red=over, yellow=not over)
        for nodeid, reason in self.skips:
            terminalreporter.line(f"{nodeid}: {reason}")
        verdict = "FAILED, over the limit" if over else "within the limit"
        terminalreporter.line(f"{len(self.skips)} host timing skips, limit {self.limit()}: {verdict}")
