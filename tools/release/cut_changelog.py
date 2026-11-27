#!/usr/bin/env python3
"""Turn a changelog's unreleased section into a version section.

    cut_changelog.py cut FILE --version X.Y.Z --date YYYY-MM-DD
    cut_changelog.py check FILE --version X.Y.Z
    cut_changelog.py section FILE --version X.Y.Z

The unreleased heading is `## Unreleased` or `## [Unreleased]`. A section
runs to the next `## ` heading or the end of the file.

cut moves the unreleased entries under a new `## [X.Y.Z] - YYYY-MM-DD`
heading and leaves the unreleased heading, empty, above it. A `### ` group
that appears more than once is joined into its first place, its entries
in file order. Joined parts keep their own lines and blank lines, with
one blank line between them, or none when a list meets a list. Lines before the first group stay first. No entry is
rewritten. cut refuses an unreleased section with no entry and a version
that already has a section.

check passes when the unreleased section holds only blank lines and the
next section is that version. section prints the version's section.
cut writes a temporary file and then replaces FILE, so a failed write
leaves FILE as it was. A FILE that is not UTF-8 is an error. Errors are `::error::` lines and exit code 1. Standard library only.
"""
import argparse
import os
import re
import stat
import sys
import tempfile

UNRELEASED = re.compile(r"^## (Unreleased|\[Unreleased\])\s*$")
VERSION = re.compile(r"^\d+\.\d+\.\d+$")
DATE = re.compile(r"^\d{4}-\d{2}-\d{2}$")
LIST_ITEM = re.compile(r"^ *[-*] ")


class ChangelogError(Exception):
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


def read_lines(path):
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as e:
        raise ChangelogError(f"cannot read {path}: {e.strerror}") from None
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        raise ChangelogError(f"{path} is not UTF-8") from None
    if "\r" in text:
        raise ChangelogError(f"{path} has CR line ends")
    return text.split("\n")


def section_starts(lines):
    return [i for i, line in enumerate(lines) if line.startswith("## ")]


def unreleased_span(lines):
    """Start and end of the unreleased section: heading index, next heading."""
    starts = section_starts(lines)
    found = [i for i in starts if UNRELEASED.match(lines[i])]
    if not found:
        raise ChangelogError("no `## Unreleased` or `## [Unreleased]` heading")
    if len(found) > 1:
        raise ChangelogError("more than one unreleased heading")
    head = found[0]
    after = [i for i in starts if i > head]
    return head, after[0] if after else len(lines)


def version_heading(line, version):
    m = re.match(r"^## \[?([^\]\s]+)\]?(\s|$)", line)
    return bool(m) and m.group(1) == version


def find_version(lines, version):
    return [i for i in section_starts(lines) if version_heading(lines[i], version)]


def trim(block):
    while block and not block[0].strip():
        block = block[1:]
    while block and not block[-1].strip():
        block = block[:-1]
    return block


def ends_in_list(part):
    """True when the last block of the part, after its last blank line, is a list."""
    start = len(part)
    while start > 0 and part[start - 1].strip():
        start -= 1
    return start < len(part) and bool(LIST_ITEM.match(part[start]))


def join_parts(parts):
    """Join trimmed parts with one blank line, or none between two lists."""
    out = []
    for p in parts:
        p = trim(p)
        if not p:
            continue
        if out and not (ends_in_list(out) and LIST_ITEM.match(p[0])):
            out.append("")
        out += p
    return out


def join_groups(body):
    """Lines before the first group, then each group once, in first order."""
    lead, order, groups = [], [], {}
    part = lead
    for line in body:
        if line.startswith("### "):
            name = line.rstrip()
            if name not in groups:
                order.append(name)
                groups[name] = []
            part = []
            groups[name].append(part)
            continue
        part.append(line)
    out = []
    lead = trim(lead)
    if lead:
        out += lead + [""]
    for name in order:
        entries = join_parts(groups[name])
        if entries:
            out += [name, ""] + entries + [""]
    return out


def has_entry(body):
    return any(line.strip() and not line.startswith("### ") for line in body)


def cut(lines, version, date):
    if find_version(lines, version):
        raise ChangelogError(f"version {version} already has a section")
    head, end = unreleased_span(lines)
    body = lines[head + 1:end]
    if not has_entry(body):
        raise ChangelogError("the unreleased section has no entry")
    new = join_groups(body)
    return (lines[:head + 1] + ["", f"## [{version}] - {date}", ""] + new
            + lines[end:])


def check(lines, version):
    head, end = unreleased_span(lines)
    if any(line.strip() for line in lines[head + 1:end]):
        raise ChangelogError("the unreleased section is not empty")
    if end == len(lines) or not version_heading(lines[end], version):
        raise ChangelogError(f"the section after the unreleased one is not {version}")


def section(lines, version):
    found = find_version(lines, version)
    if not found:
        raise ChangelogError(f"version {version} has no section")
    if len(found) > 1:
        raise ChangelogError(f"version {version} has more than one section")
    start = found[0]
    after = [i for i in section_starts(lines) if i > start]
    return trim(lines[start:after[0] if after else len(lines)])


def run(argv):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = p.add_subparsers(dest="command", required=True)
    for name in ("cut", "check", "section"):
        s = sub.add_parser(name)
        s.add_argument("file")
        s.add_argument("--version", required=True)
        if name == "cut":
            s.add_argument("--date", required=True)
    args = p.parse_args(argv)
    try:
        if not VERSION.match(args.version):
            raise ChangelogError(f"version must be X.Y.Z, got {args.version!r}")
        lines = read_lines(args.file)
        if args.command == "cut":
            if not DATE.match(args.date):
                raise ChangelogError(f"date must be YYYY-MM-DD, got {args.date!r}")
            text = "\n".join(cut(lines, args.version, args.date))
            try:
                write_atomic(args.file, text)
            except OSError as e:
                raise ChangelogError(f"cannot write: {e.strerror}") from None
        elif args.command == "check":
            check(lines, args.version)
        else:
            print("\n".join(section(lines, args.version)))
    except ChangelogError as e:
        print(f"::error::cut_changelog: {args.file}: {e}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(run(sys.argv[1:]))
