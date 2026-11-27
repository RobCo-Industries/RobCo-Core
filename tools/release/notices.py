#!/usr/bin/env python3
"""Join the licence texts of the suite into one notices file.

    notices.py --work w --out NOTICES.txt

--work holds one checkout per repository, named as the repository:
w/RobCo-Core and the five siblings. The file gets the LICENSE of each of
the six repositories, in a fixed order, then every licence file under
w/RobCo-Core/third_party/: first the licence files directly in it, then
folder by folder, each in sorted order. Other loose files are ignored. A licence
file is one whose name starts with LICENSE, LICENCE or COPYING, in any
case. Every folder inside a package, the package folder included, gives
its own licence files, or its PROVENANCE file when it has none.
Each text comes under a line that names its repository and path.

A missing LICENSE, a missing third_party folder, a package folder that
gives no text or a folder that cannot be read fails, with an
`::error::` line and exit code 1.
The output goes through a temporary file, so a failed write leaves no
half-written file. The same inputs give the same bytes. Standard library only.
"""
import argparse
import os
import stat
import sys
import tempfile

REPOS = ("RobCo-Core", "RETROS-BIOS", "MFBootAgent", "UOS", "Pip-OS", "Deitrix")
CORE = REPOS[0]
PREFIXES = ("license", "licence", "copying")


class NoticesError(Exception):
    pass


def write_atomic(path, text):
    """Write text to a temporary file beside path, then replace path with it.

    On failure the temporary file goes and path stays as it was. An
    existing file keeps its mode."""
    folder = os.path.dirname(os.path.abspath(path))
    try:
        mode = stat.S_IMODE(os.stat(path).st_mode)
    except FileNotFoundError:
        umask = os.umask(0)
        os.umask(umask)
        mode = 0o666 & ~umask
    fd, tmp = tempfile.mkstemp(dir=folder, prefix=".", suffix=".tmp")
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="") as f:
            f.write(text)
            f.flush()
            os.fsync(f.fileno())
        os.chmod(tmp, mode)
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def read_text(path):
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as e:
        raise NoticesError(f"cannot read {path}: {e.strerror}") from None
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        raise NoticesError(f"{path} is not UTF-8") from None
    return text.replace("\r\n", "\n").rstrip() + "\n"


def walk_error(e):
    raise NoticesError(f"cannot read {e.filename}: {e.strerror}")


def package_files(root):
    """Paths, relative to root, that a package folder contributes.

    Each folder gives its own licence files, or its PROVENANCE file when
    it has none."""
    found = []
    for folder, dirs, files in os.walk(root, onerror=walk_error):
        dirs.sort()
        own = [n for n in files if n.lower().startswith(PREFIXES)]
        if not own and "PROVENANCE" in files:
            own = ["PROVENANCE"]
        found += [os.path.relpath(os.path.join(folder, n), root) for n in own]
    return sorted(p.replace(os.sep, "/") for p in found)


def collect(work):
    parts = []
    for repo in REPOS:
        parts.append((repo, "LICENSE", os.path.join(work, repo, "LICENSE")))
    third = os.path.join(work, CORE, "third_party")
    if not os.path.isdir(third):
        raise NoticesError(f"no folder {third}")
    try:
        names = sorted(os.listdir(third))
    except OSError as e:
        raise NoticesError(f"cannot read {third}: {e.strerror}") from None
    paths = [n for n in names
             if os.path.isfile(os.path.join(third, n)) and n.lower().startswith(PREFIXES)]
    for name in names:
        folder = os.path.join(third, name)
        if not os.path.isdir(folder):
            continue
        found = package_files(folder)
        if not found:
            raise NoticesError(f"third_party/{name} has no licence file and no PROVENANCE")
        paths += [f"{name}/{rel}" for rel in found]
    if not paths:
        raise NoticesError(f"no licence file under {third}")
    for rel in paths:
        parts.append((CORE, f"third_party/{rel}", os.path.join(third, rel)))
    return parts


def render(parts):
    out = ["Licence notices of the RobCo Industries suite.\n"]
    for repo, rel, path in parts:
        out.append(f"\n==== {repo}: {rel} ====\n\n{read_text(path)}")
    return "".join(out)


def run(argv):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--work", required=True)
    p.add_argument("--out", required=True)
    args = p.parse_args(argv)
    try:
        text = render(collect(args.work))
        try:
            write_atomic(args.out, text)
        except OSError as e:
            raise NoticesError(f"cannot write {args.out}: {e.strerror}") from None
    except NoticesError as e:
        print(f"::error::notices: {e}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(run(sys.argv[1:]))
