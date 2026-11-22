#!/usr/bin/env python3
"""Check the Markdown documentation against the repository.

    check_docs.py [--root DIR] [--no-commands]

Fails (exit 1) when:
  - a relative link or image in a tracked .md file does not resolve,
    including #anchor targets, using GitHub's heading anchors;
  - a function declared in include/robco/*.h is missing from
    docs/guide/API.md, or API.md declares a function no header has;
  - a fenced block after <!-- check: runs --> in
    docs/guide/GETTING_STARTED.md fails when run from the root;
  - a tracked .md file contains an em dash, an emoji or a banned word.

Stdlib only. --no-commands skips the command check.
"""

import argparse
import os
import re
import subprocess
import sys
import unicodedata
from urllib.parse import unquote

API_MD = "docs/guide/API.md"
GUIDE_MD = "docs/guide/GETTING_STARTED.md"
HEADER_DIR = "include/robco"

# Files the repo does not author. Their text follows upstream.
EXCLUDE_PREFIXES = ("third_party/",)

# Header functions API.md need not list. Keep this short: each entry
# needs a reason.
IGNORE_FUNCTIONS = {
    # Called only by the IRQ vector in src/arch/vectors.S.
    "robco_irq_dispatch",
}

BANNED_WORDS = re.compile(
    r"\b(claude|copilot|ai-generated|milestones?|workers?)\b", re.IGNORECASE)
EM_DASH = "—"
EMOJI = re.compile("[\U0001F000-\U0001FAFF☀-➿️]")

CHECK_MARK = "<!-- check: runs -->"
FENCE = re.compile(r"^\s*(```+|~~~+)(.*)$")
LINK = re.compile(r"!?\[(?:[^\[\]]|\[[^\]]*\])*\]\(\s*<?([^)\s>]+)>?(?:\s+\"[^\"]*\")?\s*\)")
HEADING = re.compile(r"^\s{0,3}(#{1,6})\s+(.*?)\s*#*\s*$")
C_KEYWORDS = {"return", "if", "while", "for", "switch", "else", "case", "sizeof",
              "do", "typedef", "struct", "enum", "union", "_Static_assert",
              "asm", "__asm__", "volatile"}


def tracked_markdown(root):
    """Tracked .md files, relative to root. Falls back to a walk without git."""
    try:
        out = subprocess.run(["git", "ls-files", "-z", "*.md"], cwd=root, check=True,
                             capture_output=True, text=True).stdout
        files = [f for f in out.split("\0") if f]
    except (OSError, subprocess.CalledProcessError):
        files = []
        for d, dirs, names in os.walk(root):
            dirs[:] = [x for x in dirs if not x.startswith(".") and x != "build"]
            for n in names:
                if n.endswith(".md"):
                    files.append(os.path.relpath(os.path.join(d, n), root))
    return sorted(f for f in files if not f.startswith(EXCLUDE_PREFIXES))


def prose_lines(text):
    """Yields (line number, line, in_fence) for each line."""
    fence = None
    for n, line in enumerate(text.splitlines(), 1):
        m = FENCE.match(line)
        if m:
            mark = m.group(1)
            if fence is None:
                fence = mark[0] * len(mark)
                yield n, line, True
                continue
            if mark.startswith(fence) and not m.group(2).strip():
                fence = None
                yield n, line, True
                continue
        yield n, line, fence is not None


def strip_inline_code(line):
    return re.sub(r"(`+)(.+?)\1", "", line)


def slugify(heading):
    """GitHub's anchor for a heading text."""
    text = re.sub(r"!?\[([^\]]*)\]\([^)]*\)", r"\1", heading)  # links keep their text
    text = re.sub(r"<[^>]+>", "", text)                          # inline HTML
    text = text.replace("`", "")
    text = text.strip().lower()
    out = []
    for ch in text:
        if ch in " -_" or ch.isalnum() or unicodedata.category(ch) in ("Mn", "Pc"):
            out.append("-" if ch == " " else ch)
    return "".join(out)


def anchors(text):
    seen = {}
    result = set()
    for _, line, in_fence in prose_lines(text):
        if in_fence:
            continue
        m = HEADING.match(line)
        if not m:
            continue
        slug = slugify(m.group(2))
        n = seen.get(slug, 0)
        seen[slug] = n + 1
        result.add(slug if n == 0 else f"{slug}-{n}")
    return result


def check_links(root, files):
    errors = []
    cache = {}

    def anchors_of(path):
        if path not in cache:
            with open(path, encoding="utf-8") as f:
                cache[path] = anchors(f.read())
        return cache[path]

    for rel in files:
        path = os.path.join(root, rel)
        with open(path, encoding="utf-8") as f:
            text = f.read()
        for n, line, in_fence in prose_lines(text):
            if in_fence:
                continue
            for m in LINK.finditer(strip_inline_code(line)):
                target = m.group(1)
                if re.match(r"^[a-z][a-z0-9+.-]*:", target, re.I) or target.startswith("//"):
                    continue
                file_part, _, anchor = target.partition("#")
                file_part = unquote(file_part)
                if file_part:
                    dest = os.path.normpath(os.path.join(os.path.dirname(path), file_part))
                else:
                    dest = path
                if not os.path.exists(dest):
                    errors.append(f"{rel}:{n}: broken link {target}")
                    continue
                if anchor and dest.endswith(".md") and os.path.isfile(dest):
                    if unquote(anchor).lower() not in anchors_of(dest):
                        errors.append(f"{rel}:{n}: no heading for #{anchor} in "
                                      f"{os.path.relpath(dest, root)}")
    return errors


def strip_c_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def drop_preprocessor(text):
    out = []
    cont = False
    for line in text.splitlines():
        if cont or line.lstrip().startswith("#"):
            cont = line.rstrip().endswith("\\")
            continue
        out.append(line)
    return "\n".join(out)


PROTO = re.compile(r"^((?:[A-Za-z_]\w*[\s*]+)+?)\**([A-Za-z_]\w*)\s*\(")


def declared_functions(text, inline_bodies):
    """Function names declared in C text: `type name(...);`.

    With inline_bodies, `static inline type name(...) {` counts too.
    """
    names = set()
    text = drop_preprocessor(strip_c_comments(text))
    start = 0
    for m in re.finditer(r"[;{}]", text):
        stmt = text[start:m.start()].strip()
        end = m.group(0)
        start = m.end()
        if end == "}" or not stmt:
            continue
        p = PROTO.match(stmt)
        if not p:
            continue
        words = p.group(1).split()
        if words[0] in C_KEYWORDS or p.group(2) in C_KEYWORDS:
            continue
        if any(w.strip("*") in C_KEYWORDS for w in words):
            continue
        if "=" in stmt.split("(", 1)[0]:
            continue
        if end == "{" and not (inline_bodies and "inline" in words):
            continue
        names.add(p.group(2))
    return names


def header_functions(root):
    names = set()
    d = os.path.join(root, HEADER_DIR)
    for n in sorted(os.listdir(d)):
        if n.endswith(".h"):
            with open(os.path.join(d, n), encoding="utf-8") as f:
                names |= declared_functions(f.read(), inline_bodies=True)
    return names


def api_functions(text):
    """Functions API.md declares in its fenced code blocks."""
    code = []
    for _, line, in_fence in prose_lines(text):
        if in_fence and not FENCE.match(line):
            code.append(line)
        elif not in_fence:
            code.append(";")
    return declared_functions("\n".join(code), inline_bodies=True)


def check_api(root):
    api_path = os.path.join(root, API_MD)
    if not os.path.exists(api_path):
        return [f"{API_MD}: missing"]
    with open(api_path, encoding="utf-8") as f:
        documented = api_functions(f.read())
    declared = header_functions(root)
    errors = [f"{API_MD}: {n}() is declared in {HEADER_DIR} but not documented"
              for n in sorted(declared - documented - IGNORE_FUNCTIONS)]
    errors += [f"{API_MD}: {n}() is documented but no header declares it"
               for n in sorted(documented - declared)]
    return errors


def marked_commands(text):
    """(line, command) for each line of a fenced block after the check marker."""
    cmds = []
    armed = False
    inside = False
    for n, line, in_fence in prose_lines(text):
        if not in_fence:
            inside = False
            if line.strip() == CHECK_MARK:
                armed = True
            elif line.strip():
                armed = False
            continue
        if FENCE.match(line) and not inside:
            inside = armed
            armed = False
            continue
        if inside and not FENCE.match(line):
            cmd = line.strip()
            if cmd and not cmd.startswith("#"):
                cmds.append((n, cmd))
    return cmds


def check_commands(root, rel=GUIDE_MD):
    path = os.path.join(root, rel)
    if not os.path.exists(path):
        return []
    with open(path, encoding="utf-8") as f:
        cmds = marked_commands(f.read())
    errors = []
    for n, cmd in cmds:
        r = subprocess.run(["sh", "-c", cmd], cwd=root, capture_output=True, text=True)
        if r.returncode != 0:
            tail = (r.stderr or r.stdout).strip().splitlines()[-3:]
            errors.append(f"{rel}:{n}: `{cmd}` exited {r.returncode}: " + " / ".join(tail))
    return errors


def check_words(root, files):
    errors = []
    for rel in files:
        with open(os.path.join(root, rel), encoding="utf-8") as f:
            for n, line in enumerate(f, 1):
                if EM_DASH in line:
                    errors.append(f"{rel}:{n}: em dash")
                if EMOJI.search(line):
                    errors.append(f"{rel}:{n}: emoji")
                for m in BANNED_WORDS.finditer(line):
                    errors.append(f"{rel}:{n}: banned word '{m.group(0)}'")
    return errors


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    p.add_argument("--no-commands", action="store_true")
    args = p.parse_args(argv)

    files = tracked_markdown(args.root)
    errors = check_links(args.root, files)
    errors += check_api(args.root)
    errors += check_words(args.root, files)
    if not args.no_commands:
        errors += check_commands(args.root)
    for e in errors:
        print(e, file=sys.stderr)
    if errors:
        print(f"check_docs: {len(errors)} problem(s)", file=sys.stderr)
        return 1
    print(f"check_docs: {len(files)} files, {len(header_functions(args.root))} functions, ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
