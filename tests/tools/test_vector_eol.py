"""Every file under tests/vectors/ keeps its bytes on any checkout.

A checkout with core.autocrlf=true turns LF into CRLF in a file git takes
for text. .gitattributes sets -text there, so `git check-attr text` must
answer `unset` for each tracked file.
"""

import os
import shutil
import subprocess

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def git(*args):
    return subprocess.run(["git", "-C", ROOT, *args], capture_output=True, text=True,
                          check=True).stdout


def test_vector_files_have_no_eol_conversion():
    if shutil.which("git") is None or not os.path.exists(os.path.join(ROOT, ".git")):
        pytest.skip("not a git checkout: no .git at the repository root")
    if subprocess.run(["git", "-C", ROOT, "rev-parse", "--git-dir"],
                      capture_output=True).returncode != 0:
        pytest.skip("the .git at the repository root points to no repository here")
    files = git("ls-files", "-z", "tests/vectors").split("\0")
    files = [f for f in files if f]
    assert files, "no tracked file under tests/vectors"
    out = git("check-attr", "-z", "text", "--", *files).split("\0")
    attrs = dict(zip(out[0::3], out[2::3]))
    bad = sorted(f for f in files if attrs.get(f) != "unset")
    assert not bad, f"text not unset for: {bad}"
