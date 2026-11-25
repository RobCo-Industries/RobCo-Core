"""Fails a target session where one board skips too many tests for host timing.

A host timing skip is one made through harness.hosttiming.skip(). It
counts against its test's board parameter; tests with no board share
one bucket. Each board allows one, or --timing-skips-max=N per board.
"""

import collections

import pytest

from harness.hosttiming import is_timing_skip

NO_BOARD = "(no board)"


def pytest_addoption(parser):
    parser.addoption("--timing-skips-max", type=int, default=1,
                     help="host timing skips allowed per board (default: 1)")


def pytest_configure(config):
    config.pluginmanager.register(TimingSkips(config), "robco-timing-skips")


class TimingSkips:
    def __init__(self, config):
        self.config = config
        self.boards = {}
        self.skips = []

    def limit(self):
        return self.config.getoption("--timing-skips-max")

    def counts(self):
        return collections.Counter(board for _, board, _ in self.skips)

    def over(self):
        return sorted(b for b, n in self.counts().items() if n > self.limit())

    def pytest_collection_modifyitems(self, items):
        for item in items:
            callspec = getattr(item, "callspec", None)
            board = callspec.params.get("board") if callspec else None
            self.boards[item.nodeid] = board or NO_BOARD

    def pytest_runtest_logreport(self, report):
        if report.skipped and isinstance(report.longrepr, tuple) and is_timing_skip(report.longrepr[2]):
            board = self.boards.get(report.nodeid, NO_BOARD)
            self.skips.append((report.nodeid, board, report.longrepr[2]))

    def pytest_sessionfinish(self, session):
        if self.over() and session.exitstatus == pytest.ExitCode.OK:
            session.exitstatus = pytest.ExitCode.TESTS_FAILED

    def pytest_terminal_summary(self, terminalreporter):
        if not self.skips:
            return
        over = self.over()
        terminalreporter.section("host timing skips", red=bool(over), yellow=not over)
        for nodeid, board, reason in self.skips:
            terminalreporter.line(f"{board}: {nodeid}: {reason}")
        counts = ", ".join(f"{b} {n}" for b, n in sorted(self.counts().items()))
        verdict = f"FAILED, over the limit: {', '.join(over)}" if over else "within the limit"
        terminalreporter.line(
            f"{len(self.skips)} host timing skips ({counts}), limit {self.limit()} per board: {verdict}")
