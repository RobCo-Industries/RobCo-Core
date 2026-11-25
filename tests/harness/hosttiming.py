"""Skips a test because the host timing of its runs was too poor to judge.

Every such skip carries the reason prefix PREFIX, so tests/target/conftest.py
can count them and fail a session that skips too many.
"""

import pytest

PREFIX = "host timing:"


def skip(numbers):
    """Skips the test. `numbers` says what the host timing was."""
    pytest.skip(f"{PREFIX} {numbers}")


def is_timing_skip(reason):
    """True when a skip reason, with or without pytest's "Skipped: ", is a host timing skip."""
    if reason.startswith("Skipped: "):
        reason = reason[len("Skipped: "):]
    return reason.startswith(PREFIX)
