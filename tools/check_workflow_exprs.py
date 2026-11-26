#!/usr/bin/env python3
"""Fail when a workflow line holds a ${{ ... }} expression in an unsafe shape.

Actions pastes the value of ${{ ... }} into a run: script before the shell
reads it, so a tag or a pull request title such as $(id) would run as a
command. Every value must reach a script through env: and the script reads
"$NAME".

The guard catches an honest slip by a person or a tool that writes
ordinary workflow YAML. It is not a defence against YAML made unusual on
purpose: whoever writes that can change the workflow anyway.

This check does not read YAML. It judges every line that holds ${{,
comments included, and fails closed. A line passes only in this shape:
optional indentation, an optional "- ", a key of letters, digits, _ or -,
then ": ", then a value that holds each expression whole, ${{ and }} on
that line. The key must not name code: run, script, shell, args or
entrypoint, in any case. The value must not start with [ or {, and with
the expressions cut out it holds no " and no '. The nearest line above
that is indented less, counting a list item's key from after its "- ",
must end in a colon and hold no value, so the line is not the tail of a
multi-line value. Every other shape fails, a comment included, since a
shell comment inside a script is still filled in.

A line that ends in | or > after a key and a colon opens a block scalar,
whatever stands between the colon and the indicator. Every following line
indented deeper than that key, blank lines included, is block text, and
any ${{ there fails. An anchor, an alias or a merge key on a structure
line fails the whole file with one line: anchors and aliases are not read.
A structure line is any line that is not block text. The token counts only
where YAML reads one: at the start of a value, right after "KEY: " or "- ",
as in "run: &script |", "uses: *step" or "<<: *base". A * or & further
inside a value, as in "run: ls *.zip", or in block text is not a token.

What stays unseen: a value that reaches a script through env: and is then
used unquoted or in eval; a reusable workflow or an action in another
repository; values relayed through outputs and used by a later script; a
multi-line quoted value whose inner lines look like a mapping, which is read
as structure; and any YAML form made to look like the safe shape on purpose.

A second rule checks each uses: line. An action in another repository
names a full 40 hex digit commit, followed by a comment with its tag:
"uses: actions/checkout@<commit> # v4.4.0". A tag or a branch can move
under the workflow; a commit cannot. A reusable workflow called by path,
"OWNER/REPO/.github/workflows/FILE@REF", names a full commit too; it
needs no comment, since check_pins.sh ties it to the submodule. A
docker:// image names a sha256 digest. A local "./" action or workflow
passes: it runs at the caller's own commit. Any other uses: value fails.

The second rule fails closed too. Any structure line that holds the word
uses, in any case, outside a comment, must take one plain form: spaces as
indent, an optional "- ", the bare key uses, a colon, one space, an
unquoted reference, and an optional " # comment". The line must sit under a
bare "KEY:" line, as for the first rule, and no deeper line may continue
the value. Every other spelling fails as a form the guard cannot check: a
space before the colon, a quoted key or value, a flow mapping or sequence,
a tab, a block scalar, a multi-line value, uses after other content, and
the word uses in any other key or value. An anchor, an alias, a merge key,
a second YAML document, a byte order mark or a carriage return fails the
file when it holds the word uses.

The rule does not check container: or services: images, nor any image:
key. Their tags can move.

Usage: check_workflow_exprs.py [--no-pins] FILE...
--no-pins skips the second rule.
Exit 0 when clean, 1 with one line per hit, 2 on a usage error.
"""

import re
import sys

SAFE = re.compile(r"^[ \t]*(?:- )?([A-Za-z0-9_-]+): (.*)$")
BLOCK = re.compile(r"^([ \t]*(?:- )?)[A-Za-z0-9_\"'-][^:]*:(?:[ \t]+\S+)*?[ \t]*[|>][-+0-9]*[ \t]*(?:#.*)?$")
ANCHOR = re.compile(
    r"^[ \t]*(?:- )*(?:(?:[^\s#:][^:#]*)?:[ \t]+)?(?:![^\s]*[ \t]+)?[&*][A-Za-z0-9_-]"
    r"|^[ \t]*(?:- )*<<[ \t]*:"
)
EXPR = re.compile(r"\$\{\{.*?\}\}")
LEAD = re.compile(r"^[ \t]*(?:- )*")
OPENER = re.compile(r"^[^#]*:[ \t]*(?:#.*)?$")
CODE_KEYS = {"run", "script", "shell", "args", "entrypoint"}


def whole_exprs(value):
    """True when every ${{ in value has a }} after it on the same line."""
    pos = 0
    while True:
        start = value.find("${{", pos)
        if start < 0:
            return True
        end = value.find("}}", start + 3)
        if end < 0:
            return False
        pos = end + 2


def key_indent(line):
    """Column of the key, after any list item "- "."""
    return len(LEAD.match(line).group(0))


def under_opener(lines, index):
    """True when the nearest less-indented line above is "KEY:" alone."""
    own = key_indent(lines[index])
    for line in reversed(lines[:index]):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        if key_indent(line) < own:
            return bool(OPENER.match(line))
    return True


def block_text_lines(all_lines):
    """Line numbers of block scalar text."""
    found = set()
    block_indent = None
    for num, line in enumerate(all_lines, 1):
        indent = len(line) - len(line.lstrip(" \t"))
        if block_indent is not None:
            if not line.strip() or indent > block_indent:
                found.add(num)
                continue
            block_indent = None
        block = BLOCK.match(line)
        if block:
            block_indent = len(block.group(1))
    return found


def findings(text):
    """Yield (line number, reason) for each unsafe line."""
    all_lines = text.splitlines()
    text_lines = block_text_lines(all_lines)
    for num, line in enumerate(all_lines, 1):
        if num not in text_lines and ANCHOR.match(line):
            yield num, "anchors and aliases are not read"
            return
    block_indent = None
    for num, line in enumerate(all_lines, 1):
        indent = len(line) - len(line.lstrip(" \t"))
        if block_indent is not None:
            if not line.strip() or indent > block_indent:
                if "${{" in line:
                    yield num, "${{ inside a block scalar"
                continue
            block_indent = None
        block = BLOCK.match(line)
        if block:
            block_indent = len(block.group(1))
        if "${{" not in line:
            continue
        match = SAFE.match(line)
        if not match:
            yield num, "${{ in a line that is not KEY: value"
        elif match.group(1).lower() in CODE_KEYS:
            yield num, "${{ in a %s: value" % match.group(1)
        elif not whole_exprs(match.group(2)):
            yield num, "${{ not closed on its line"
        elif match.group(2).startswith(("[", "{")):
            yield num, "${{ in a flow collection"
        elif re.search("[\"']", EXPR.sub("", match.group(2))):
            yield num, "${{ in a line with quotes outside the expression"
        elif not under_opener(all_lines, num - 1):
            yield num, "${{ in a continuation line"


USES_WORD = re.compile(r"\buses\b", re.IGNORECASE)
COMMENT = re.compile(r"(?:^|[ \t])#")
PLAIN_USES = re.compile(r"^( *)(?:- )?uses: ([^\s\"'#{}\[\],&*!|>%@`][^\s\"'#{}\[\],]*)(.*)$")
TAIL = re.compile(r"^(?: #.*)?$")
DOC_MARK = re.compile(r"^(?:---|\.\.\.)(?:[ \t].*)?$")
COMMIT = re.compile(r"@[0-9a-f]{40}$")
DIGEST = re.compile(r"@sha256:[0-9a-f]{64}$")
TAG_COMMENT = re.compile(r"^[ \t]+#[ \t]*\S")
FORM = "uses: in a form the guard cannot check"


def code_part(line):
    """The line up to its comment, if any."""
    found = COMMENT.search(line)
    return line[: found.start()] if found else line


def continues(all_lines, index, key_col):
    """True when a line below is indented deeper than the key: the value goes on."""
    for line in all_lines[index + 1:]:
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        return len(line) - len(line.lstrip(" \t")) > key_col
    return False


def pin_findings(text):
    """Yield (line number, reason) for each uses line not plain or not pinned.

    A line triggers when the word uses stands outside a comment and outside
    block text, which block_text_lines finds as for the first rule.
    """
    all_lines = text.split("\n")
    if all_lines and all_lines[-1] == "":
        all_lines.pop()
    text_lines = block_text_lines(all_lines)
    structure = [n for n in range(1, len(all_lines) + 1) if n not in text_lines]
    triggers = [n for n in structure if USES_WORD.search(code_part(all_lines[n - 1]))]
    if not triggers:
        return
    whole = None
    if text.startswith("\ufeff"):
        whole = "a byte order mark"
    elif "\r" in text:
        whole = "a carriage return"
    elif any(ANCHOR.match(all_lines[n - 1]) for n in structure):
        whole = "an anchor, an alias or a merge key"
    elif [n for n in structure if DOC_MARK.match(all_lines[n - 1])] not in ([], [1]):
        whole = "more than one YAML document"
    if whole:
        yield triggers[0], "%s with %s in the file" % (FORM, whole)
        return
    for num in triggers:
        line = all_lines[num - 1]
        match = PLAIN_USES.match(line)
        if (
            not match
            or not TAIL.match(match.group(3))
            or not under_opener(all_lines, num - 1)
            or continues(all_lines, num - 1, key_indent(line))
        ):
            yield num, FORM
            continue
        ref, rest = match.group(2), match.group(3)
        if ref.startswith("./"):
            continue
        if ref.startswith("docker://"):
            if not DIGEST.search(ref):
                yield num, "docker image not pinned by a sha256 digest"
        elif not COMMIT.search(ref):
            yield num, "uses: not pinned by a full commit"
        elif "/.github/workflows/" not in ref and not TAG_COMMENT.match(rest):
            yield num, "pinned action has no tag comment"


def main(argv):
    pins = True
    if argv[:1] == ["--no-pins"]:
        pins = False
        argv = argv[1:]
    if not argv:
        print("usage: check_workflow_exprs.py [--no-pins] FILE...", file=sys.stderr)
        return 2
    bad = 0
    for path in argv:
        with open(path, encoding="utf-8") as handle:
            text = handle.read()
        for num, reason in findings(text):
            print("%s:%d: %s; pass the value through env:" % (path, num, reason))
            bad += 1
        if pins:
            for num, reason in pin_findings(text):
                print(
                    "%s:%d: %s; write uses: OWNER/REPO@<commit> # <tag>, "
                    "indented with spaces, one space after the colon" % (path, num, reason)
                )
                bad += 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
