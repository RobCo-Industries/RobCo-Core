#!/usr/bin/env python3
"""Join the licence texts of the suite into one notices file.

    notices.py --work w --out NOTICES.txt

--work holds one checkout per repository, named as the repository:
w/RobCo-Core and the five siblings. The file gets the LICENSE of each of
the six repositories, in a fixed order, then every licence file under
w/RobCo-Core/third_party/, folder by folder in sorted order. A licence
file is one whose name starts with LICENSE, LICENCE or COPYING, in any
case. A folder with no licence file gives its PROVENANCE file instead.
Each text comes under a line that names its repository and path.

A missing LICENSE, a missing third_party folder or a third_party folder
with neither a licence file nor a PROVENANCE file fails, with an `::error::` line and exit code 1.
The same inputs give the same bytes. Standard library only.
"""
import argparse
import os
import sys

REPOS = ("RobCo-Core", "RETROS-BIOS", "MFBootAgent", "UOS", "Pip-OS", "Deitrix")
CORE = REPOS[0]
PREFIXES = ("license", "licence", "copying")


class NoticesError(Exception):
    pass


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


def licence_files(root):
    found = []
    for folder, dirs, files in os.walk(root):
        dirs.sort()
        for name in sorted(files):
            if name.lower().startswith(PREFIXES):
                found.append(os.path.relpath(os.path.join(folder, name), root))
    return sorted(p.replace(os.sep, "/") for p in found)


def collect(work):
    parts = []
    for repo in REPOS:
        parts.append((repo, "LICENSE", os.path.join(work, repo, "LICENSE")))
    third = os.path.join(work, CORE, "third_party")
    if not os.path.isdir(third):
        raise NoticesError(f"no folder {third}")
    paths = []
    for name in sorted(os.listdir(third)):
        folder = os.path.join(third, name)
        if not os.path.isdir(folder):
            continue
        found = licence_files(folder)
        if not found and os.path.isfile(os.path.join(folder, "PROVENANCE")):
            found = ["PROVENANCE"]
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
    except NoticesError as e:
        print(f"::error::notices: {e}")
        return 1
    with open(args.out, "w", encoding="utf-8", newline="") as f:
        f.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(run(sys.argv[1:]))
