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
in file order. Lines before the first group stay first. No entry is
rewritten. cut refuses an unreleased section with no entry and a version
that already has a section.

check passes when the unreleased section holds only blank lines and the
next section is that version. section prints the version's section.
Errors are `::error::` lines and exit code 1. Standard library only.
"""
import argparse
import re
import sys

UNRELEASED = re.compile(r"^## (Unreleased|\[Unreleased\])\s*$")
VERSION = re.compile(r"^\d+\.\d+\.\d+$")
DATE = re.compile(r"^\d{4}-\d{2}-\d{2}$")


class ChangelogError(Exception):
    pass


def read_lines(path):
    try:
        with open(path, encoding="utf-8", newline="") as f:
            text = f.read()
    except OSError as e:
        raise ChangelogError(f"cannot read {path}: {e.strerror}") from None
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
        entries = [line for p in groups[name] for line in trim(p)]
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
            with open(args.file, "w", encoding="utf-8", newline="") as f:
                f.write(text)
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
