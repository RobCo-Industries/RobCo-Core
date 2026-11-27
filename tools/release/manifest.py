#!/usr/bin/env python3
"""Write and check a suite release manifest.

    manifest.py new --suite NAME --work w --out F
    manifest.py check --manifest F --work w [--rehearsal] [--tag NAME] [--github-output F]
    manifest.py verify-night --manifest F --versions VERSIONS.txt
    manifest.py remote-tags --manifest F [--require] [--remote REPO=URL ...]
    manifest.py tags --manifest F

A manifest names one commit and one version for each of the six
repositories. --work holds one checkout per repository, named as the
repository, as for tools/nightly/plan.py. check runs every rule and
prints one ::error:: line per broken rule before it exits. remote-tags
is the only command that talks to a remote. tags prints the tag and
push commands and runs nothing. Standard library only.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

CORE = "RobCo-Core"
# Repository, default branch, output name of its commit.
REPOS = (
    (CORE, "main", "core_sha"),
    ("RETROS-BIOS", "main", "retros_sha"),
    ("MFBootAgent", "master", "mfboot_sha"),
    ("UOS", "master", "uos_sha"),
    ("Pip-OS", "master", "pipos_sha"),
    ("Deitrix", "master", "deitrix_sha"),
)
NAMES = tuple(r[0] for r in REPOS)
DEFAULT_BRANCH = {r[0]: r[1] for r in REPOS}
OUTPUT = {r[0]: r[2] for r in REPOS}
# The siblings whose own gitlink pins the library.
LIBRARY_PINNERS = ("RETROS-BIOS", "MFBootAgent", "UOS", "Pip-OS")
REMOTE = "https://github.com/RobCo-Industries/{}.git"

SUITE_RE = re.compile(r"suite-\d{4}\.(0[1-9]|1[0-2])(\.[1-9]\d*)?")
VERSION_RE = re.compile(r"(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)")
COMMIT_RE = re.compile(r"[0-9a-f]{40}")


class ManifestError(Exception):
    pass


def git(repo, *args):
    proc = subprocess.run(["git", "-C", repo, *args], capture_output=True, text=True)
    if proc.returncode != 0:
        raise ManifestError(f"git {' '.join(args)} failed in {repo}: {proc.stderr.strip()}")
    return proc.stdout.strip()


def at_commit(repo, name, commit, path):
    """The text of path in commit, never the working tree."""
    proc = subprocess.run(["git", "-C", repo, "show", f"{commit}:{path}"],
                          capture_output=True)
    if proc.returncode != 0:
        raise ManifestError(f"{name}: {commit[:12]} has no {path}")
    try:
        return proc.stdout.decode("utf-8")
    except UnicodeDecodeError:
        raise ManifestError(f"{name}: {commit[:12]}: {path} is not UTF-8") from None


def gitlink(repo, name, commit, path):
    """The commit a gitlink in commit pins."""
    out = git(repo, "ls-tree", commit, "--", path)
    mode, _, rest = out.partition(" ")
    kind, _, rest = rest.partition(" ")
    sha, _, found = rest.partition("\t")
    if mode != "160000" or kind != "commit" or found != path:
        raise ManifestError(f"{name}: {commit[:12]} has no {path} gitlink")
    return sha


def read_text(path):
    try:
        with open(path, encoding="utf-8") as f:
            return f.read()
    except OSError as e:
        raise ManifestError(f"cannot read {path}: {e.strerror}") from None


def write_output(path, text, append=False):
    """Write an output file, or raise the tool's error with the path.

    A plain write goes to a temporary file in the same folder, then
    replaces path, so the file is whole or not there at all. An append
    stays an append, as GitHub expects of its output file.
    """
    tmp = None
    try:
        if append:
            with open(path, "a", encoding="utf-8") as f:
                f.write(text)
            return
        folder = os.path.dirname(os.path.abspath(path))
        fd, tmp = tempfile.mkstemp(prefix=".manifest-", dir=folder)
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(text)
        os.replace(tmp, path)
        tmp = None
    except OSError as e:
        raise ManifestError(f"cannot write {path}: {e.strerror}") from None
    finally:
        if tmp is not None:
            try:
                os.unlink(tmp)
            except OSError:
                pass


# Version in code.

def c_define(text, name):
    m = re.search(rf"^\s*#\s*define\s+{name}\s+(.+?)\s*$", text, re.M)
    if not m:
        raise ManifestError(f"no {name} line")
    return m.group(1)


def code_version(work, name, commit):
    """The version the code of commit holds, or None for RobCo-Core."""
    repo = os.path.join(work, name)

    def read_text(path):
        return at_commit(repo, name, commit, path)
    try:
        if name == "RETROS-BIOS":
            value = c_define(read_text("src/robco_config.h"),
                             "RETROS_VERSION_STR")
        elif name == "MFBootAgent":
            value = c_define(read_text("src/robco_config.h"),
                             "MFB_VERSION_STR")
        elif name == "UOS":
            value = c_define(read_text("src/kernel/robco_config.h"),
                             "UOS_VERSION_STR")
        elif name == "Pip-OS":
            text = read_text("kernel/robco_config.h")
            return ".".join(c_define(text, f"PIPOS_VERSION_{p}")
                            for p in ("MAJOR", "MINOR", "PATCH"))
        elif name == "Deitrix":
            m = re.search(r"^VERSION\s*:=\s*(\S+)\s*$",
                          read_text("Makefile"), re.M)
            if not m:
                raise ManifestError("no VERSION := line in Makefile")
            return m.group(1)
        else:
            return None
    except ManifestError as e:
        msg = str(e)
        if msg.startswith(f"{name}: "):
            msg = msg[len(name) + 2:]
        raise ManifestError(f"{name}: version in code: {msg}") from None
    if not (value.startswith('"') and value.endswith('"')):
        raise ManifestError(f"{name}: version in code is not a string: {value}")
    return value[1:-1]


# Changelog.

HEADING_RE = re.compile(r"^##\s+(.*?)\s*$")


def changelog_sections(text):
    """[(heading, body lines)] for each level-two heading."""
    sections = []
    for line in text.splitlines():
        m = HEADING_RE.match(line)
        if m:
            sections.append((m.group(1), []))
        elif sections:
            sections[-1][1].append(line)
    return sections


def is_unreleased(heading):
    return heading in ("Unreleased", "[Unreleased]")


def heading_version(heading):
    """The version a release heading names: "[0.1.0] - 2026-12-01" or "0.1.0"."""
    m = re.match(r"\[([^\]]+)\]|(\S+)", heading)
    return (m.group(1) or m.group(2)) if m else None


def changelog_problems(text, version):
    sections = changelog_sections(text)
    for i, (heading, body) in enumerate(sections):
        if is_unreleased(heading):
            problems = []
            if any(line.strip() for line in body):
                problems.append("the Unreleased section is not empty")
            nxt = sections[i + 1][0] if i + 1 < len(sections) else None
            if nxt is None or heading_version(nxt) != version:
                problems.append(f"the section after Unreleased is not {version}")
            return problems
    return ["no Unreleased section"]


def latest_changelog_version(text):
    for heading, _ in changelog_sections(text):
        v = heading_version(heading)
        if v and VERSION_RE.fullmatch(v):
            return v
    return None


# The manifest.

def load(path):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except OSError as e:
        raise ManifestError(f"cannot read {path}: {e.strerror}") from None
    except ValueError as e:
        raise ManifestError(f"{path} is not JSON: {e}") from None


def shape_errors(data):
    """Every problem in the manifest's shape."""
    if not isinstance(data, dict):
        return ["the manifest is not an object"]
    errors = [f"unknown key {k!r}" for k in data if k not in ("suite", "repositories")]
    suite = data.get("suite")
    if not isinstance(suite, str) or not SUITE_RE.fullmatch(suite):
        errors.append(f"suite {suite!r} is not suite-YYYY.MM or suite-YYYY.MM.N")
    repos = data.get("repositories")
    if not isinstance(repos, dict):
        return errors + ["repositories is missing or not an object"]
    errors += [f"unknown repository {k!r}" for k in repos if k not in NAMES]
    for name in NAMES:
        entry = repos.get(name)
        if entry is None:
            errors.append(f"{name} is missing")
            continue
        if not isinstance(entry, dict):
            errors.append(f"{name} is not an object")
            continue
        errors += [f"{name}: unknown key {k!r}" for k in entry if k not in ("version", "commit")]
        version, commit = entry.get("version"), entry.get("commit")
        if not isinstance(version, str) or not VERSION_RE.fullmatch(version):
            errors.append(f"{name}: version {version!r} is not MAJOR.MINOR.PATCH")
        if not isinstance(commit, str) or not COMMIT_RE.fullmatch(commit):
            errors.append(f"{name}: commit {commit!r} is not 40 lower-case hex digits")
    return errors


def load_valid(path):
    data = load(path)
    errors = shape_errors(data)
    if errors:
        raise ManifestError("\n".join(errors))
    return data


def default_ref(repo, branch):
    for ref in (f"origin/{branch}", branch):
        try:
            git(repo, "rev-parse", "--verify", "--quiet", f"{ref}^{{commit}}")
            return ref
        except ManifestError:
            continue
    return None


def on_branch(repo, commit, ref):
    proc = subprocess.run(["git", "-C", repo, "merge-base", "--is-ancestor", commit, ref],
                          capture_output=True, text=True)
    return proc.returncode == 0


def check_errors(data, work, rehearsal=False, tag=None):
    """Every broken rule of a manifest against the checkouts under work."""
    errors = shape_errors(data)
    if tag is not None and isinstance(data, dict) and data.get("suite") != tag:
        errors.append(f"the tag {tag} is not the manifest's suite {data.get('suite')!r}")
    repos = data.get("repositories") if isinstance(data, dict) else None
    if not isinstance(repos, dict):
        return errors

    def field(name, key):
        entry = repos.get(name)
        value = entry.get(key) if isinstance(entry, dict) else None
        good = COMMIT_RE if key == "commit" else VERSION_RE
        return value if isinstance(value, str) and good.fullmatch(value) else None

    missing = set()
    for name in NAMES:
        repo = os.path.join(work, name)
        commit, version = field(name, "commit"), field(name, "version")
        if commit:
            try:
                git(repo, "cat-file", "-e", f"{commit}^{{commit}}")
            except ManifestError:
                errors.append(f"{name}: the checkout has no commit {commit[:12]}")
                missing.add(name)
                commit = None
        if commit:
            try:
                branch = DEFAULT_BRANCH[name]
                ref = default_ref(repo, branch)
                if ref is None:
                    errors.append(f"{name}: no {branch} branch in the checkout")
                elif not on_branch(repo, commit, ref):
                    errors.append(f"{name}: {commit[:12]} is not on {branch}")
            except ManifestError as e:
                errors.append(f"{name}: {e}")
        if version and commit:
            try:
                found = code_version(work, name, commit)
                if found is not None and found != version:
                    errors.append(f"{name}: the code says {found}, the manifest {version}")
            except ManifestError as e:
                errors.append(str(e))
            if not rehearsal:
                try:
                    text = at_commit(repo, name, commit, "CHANGELOG.md")
                    errors += [f"{name}: CHANGELOG.md: {p}"
                               for p in changelog_problems(text, version)]
                except ManifestError as e:
                    errors.append(str(e))

    core, pipos = field(CORE, "commit"), field("Pip-OS", "commit")
    pins = [(n, "external/robco-core", CORE, core) for n in LIBRARY_PINNERS]
    pins.append(("Deitrix", "external/pip-os", "Pip-OS", pipos))
    for name, path, what, want in pins:
        at = field(name, "commit")
        if not want or not at or name in missing:
            continue
        try:
            got = gitlink(os.path.join(work, name), name, at, path)
        except ManifestError as e:
            msg = str(e)
            errors.append(msg if msg.startswith(f"{name}: ") else f"{name}: {msg}")
            continue
        if got != want:
            errors.append(f"{name}: pins {what} {got[:12]}, the manifest names {want[:12]}")
    return errors


# VERSIONS.txt of a night.

def night_rows(text):
    """{repository: [(commit cell, note cell), ...]} from a plan.py table.
    Every row of a repository is kept, so a second row cannot hide the first."""
    rows = {}
    for line in text.splitlines():
        if not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) == 8 and cells[0] in NAMES:
            rows.setdefault(cells[0], []).append((cells[2].strip("`"), cells[7]))
    return rows


def verify_night_errors(data, text):
    rows = night_rows(text)
    errors = []
    for name in NAMES:
        commit = data["repositories"][name]["commit"]
        if name not in rows:
            errors.append(f"{name}: no row in VERSIONS.txt")
            continue
        if len(rows[name]) > 1:
            errors.append(f"{name}: {len(rows[name])} rows in VERSIONS.txt, one is expected")
            continue
        short, note = rows[name][0]
        if len(short) != 12 or not commit.startswith(short):
            errors.append(f"{name}: the night built {short!r}, the manifest names {commit[:12]}")
        if note:
            errors.append(f"{name}: the night notes a pin that differs: {note}")
    return errors


# Remote tags.

def remote_table(pairs):
    table = {name: REMOTE.format(name) for name in NAMES}
    for pair in pairs or ():
        name, sep, url = pair.partition("=")
        if not sep or name not in NAMES or not url:
            raise ManifestError(f"--remote {pair!r} is not REPO=URL for a known repository")
        table[name] = url
    return table


def remote_tag(url, tag):
    """The commit the remote's tag points at, peeled, or None."""
    env = dict(os.environ, GIT_TERMINAL_PROMPT="0")
    proc = subprocess.run(["git", "ls-remote", "--tags", url, f"refs/tags/{tag}",
                           f"refs/tags/{tag}^{{}}"], capture_output=True, text=True, env=env)
    if proc.returncode != 0:
        raise ManifestError(f"git ls-remote {url} failed: {proc.stderr.strip()}")
    found = {}
    for line in proc.stdout.splitlines():
        sha, _, ref = line.partition("\t")
        found[ref] = sha
    return found.get(f"refs/tags/{tag}^{{}}") or found.get(f"refs/tags/{tag}")


# Commands.

def cmd_new(args):
    if not SUITE_RE.fullmatch(args.suite):
        raise ManifestError(f"suite {args.suite!r} is not suite-YYYY.MM or suite-YYYY.MM.N")
    repos = {}
    errors = []
    for name in NAMES:
        repo = os.path.join(args.work, name)
        try:
            commit = git(repo, "rev-parse", "HEAD")
            version = code_version(args.work, name, commit)
            if version is None:
                text = at_commit(repo, name, commit, "CHANGELOG.md")
                version = latest_changelog_version(text) or "0.1.0"
            repos[name] = {"version": version, "commit": commit}
        except ManifestError as e:
            errors.append(f"{name}: {e}" if not str(e).startswith(name) else str(e))
    if errors:
        raise ManifestError("\n".join(errors))
    data = {"suite": args.suite, "repositories": repos}
    errors = shape_errors(data)
    if errors:
        raise ManifestError("\n".join(errors))
    write_output(args.out, json.dumps(data, indent=2) + "\n")
    print(f"manifest: wrote {args.out}")
    return 0


def cmd_check(args):
    data = load(args.manifest)
    errors = check_errors(data, args.work, args.rehearsal, args.tag)
    if errors:
        raise ManifestError("\n".join(errors))
    if args.github_output:
        lines = [f"suite={data['suite']}"]
        lines += [f"{OUTPUT[name]}={data['repositories'][name]['commit']}" for name in NAMES]
        write_output(args.github_output, "".join(f"{line}\n" for line in lines), append=True)
    skipped = " The changelog rule was skipped." if args.rehearsal else ""
    print(f"manifest: {data['suite']} passes every check.{skipped}")
    return 0


def cmd_verify_night(args):
    data = load_valid(args.manifest)
    errors = verify_night_errors(data, read_text(args.versions))
    if errors:
        raise ManifestError("\n".join(errors))
    print(f"manifest: the night built the six commits of {data['suite']}")
    return 0


def cmd_remote_tags(args):
    data = load_valid(args.manifest)
    table = remote_table(args.remote)
    errors = []
    for name in NAMES:
        entry = data["repositories"][name]
        tag = f"v{entry['version']}"
        try:
            got = remote_tag(table[name], tag)
        except ManifestError as e:
            errors.append(f"{name}: {e}")
            continue
        if got is None:
            if args.require:
                errors.append(f"{name}: {tag} is missing")
            else:
                print(f"{name}: {tag} is missing")
        elif got != entry["commit"]:
            errors.append(f"{name}: {tag} is on {got[:12]}, the manifest names "
                          f"{entry['commit'][:12]}")
        else:
            print(f"{name}: {tag} is on {got[:12]}")
    if errors:
        raise ManifestError("\n".join(errors))
    return 0


def cmd_tags(args):
    data = load_valid(args.manifest)
    suite = data["suite"]
    lines = []
    for name in NAMES:
        entry = data["repositories"][name]
        tag = f"v{entry['version']}"
        lines.append(f'git -C {name} tag -a {tag} -m "{name} {tag}" {entry["commit"]}')
    for name in NAMES:
        lines.append(f"git -C {name} push origin refs/tags/v{data['repositories'][name]['version']}")
    lines.append(f"# MANIFEST_COMMIT is the {CORE} commit that holds releases/{suite}.json.")
    lines.append(f'git -C {CORE} tag -a {suite} -m "{suite}" MANIFEST_COMMIT')
    lines.append(f"git -C {CORE} push origin refs/tags/{suite}")
    print("\n".join(lines))
    return 0


def run(argv):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = p.add_subparsers(dest="command", required=True)
    s = sub.add_parser("new")
    s.add_argument("--suite", required=True)
    s.add_argument("--work", required=True)
    s.add_argument("--out", required=True)
    s = sub.add_parser("check")
    s.add_argument("--manifest", required=True)
    s.add_argument("--work", required=True)
    s.add_argument("--rehearsal", action="store_true")
    s.add_argument("--tag")
    s.add_argument("--github-output")
    s = sub.add_parser("verify-night")
    s.add_argument("--manifest", required=True)
    s.add_argument("--versions", required=True)
    s = sub.add_parser("remote-tags")
    s.add_argument("--manifest", required=True)
    s.add_argument("--require", action="store_true")
    s.add_argument("--remote", action="append", metavar="REPO=URL")
    s = sub.add_parser("tags")
    s.add_argument("--manifest", required=True)
    args = p.parse_args(argv)
    commands = {"new": cmd_new, "check": cmd_check, "verify-night": cmd_verify_night,
                "remote-tags": cmd_remote_tags, "tags": cmd_tags}
    try:
        return commands[args.command](args)
    except ManifestError as e:
        for line in str(e).splitlines():
            print(f"::error::manifest: {line}")
        return 1


if __name__ == "__main__":
    sys.exit(run(sys.argv[1:]))
