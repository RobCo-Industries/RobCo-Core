#!/usr/bin/env python3
"""Plan one nightly run: resolve the commits, the image tags and the matrices.

    plan.py --work w --pairing pins --github-output F --summary F [--versions F]

--work holds one checkout per repository, named as the repository:
w/RobCo-Core and the five siblings, each sibling with its submodules.
The script reads each checkout with git and runs each sibling's own
tools/check_pins.sh on its .github/workflows/ci.yml, which proves that
the image tag in the sibling's CI equals the ci/IMAGE_TAG the nightly
reads. A mismatch, a missing file or a failed git command fails the plan.

It builds one record per built thing: the commit, the library commit
and, for Deitrix, the Pip-OS commit it builds with, and the image tag.
Deitrix builds on the Pip-OS head, so it takes that head's library and
tag, and its row notes each value that differs from its own pin. The six
JSON matrices on --github-output, the table on --summary and --versions
all read these records: stages, card_terminal, card_pipboy, agent, uos
and pipboy. Each entry holds pairing, repo, sha, core_sha, pipos_sha and
tag, and board in the three test matrices.

Two pairings exist. "pins" builds each sibling on its own library pin.
"core-head" builds every sibling on the w/RobCo-Core commit, in the image
of that commit's ci/IMAGE_TAG, and runs the tests on raspi0 and raspi2b
only. "both" puts the pins entries first, then the core-head entries.
check_pins.sh checks the pins side only.

The ref each checkout was asked for comes from the environment, for the
table only: NIGHTLY_REF_CORE, NIGHTLY_REF_RETROS, NIGHTLY_REF_MFBOOT,
NIGHTLY_REF_UOS, NIGHTLY_REF_PIPOS and NIGHTLY_REF_DEITRIX. Empty means
the default branch. Standard library only.
"""
import argparse
import json
import os
import subprocess
import sys

CORE = "RobCo-Core"
# Repository, environment suffix of its ref, path of its RobCo-Core.
SIBLINGS = (
    ("RETROS-BIOS", "RETROS", "external/robco-core"),
    ("MFBootAgent", "MFBOOT", "external/robco-core"),
    ("UOS", "UOS", "external/robco-core"),
    ("Pip-OS", "PIPOS", "external/robco-core"),
    ("Deitrix", "DEITRIX", "external/pip-os/external/robco-core"),
)
REF_ENV = {CORE: "CORE", **{name: env for name, env, _ in SIBLINGS}}
CORE_PATH = {name: path for name, _, path in SIBLINGS}
PAIRINGS = ("both", "pins", "core-head")
# The boards the core-head pairing tests on, when the sibling lists them.
CORE_HEAD_BOARDS = ("raspi0", "raspi2b")


class PlanError(Exception):
    pass


def git(repo, *args):
    proc = subprocess.run(["git", "-C", repo, *args], capture_output=True, text=True)
    if proc.returncode != 0:
        raise PlanError(f"git {' '.join(args)} failed in {repo}: {proc.stderr.strip()}")
    return proc.stdout.strip()


def read_text(path):
    try:
        with open(path, encoding="utf-8") as f:
            return f.read()
    except OSError as e:
        raise PlanError(f"cannot read {path}: {e.strerror}") from None


def image_tag(core_dir):
    tag = read_text(os.path.join(core_dir, "ci", "IMAGE_TAG")).strip()
    if not tag:
        raise PlanError(f"{core_dir}/ci/IMAGE_TAG is empty")
    return tag


def boards(repo_dir):
    """The robco_boards line of the repository's pytest.ini."""
    path = os.path.join(repo_dir, "pytest.ini")
    for line in read_text(path).splitlines():
        key, sep, value = line.partition("=")
        if sep and key.strip() == "robco_boards":
            found = value.split()
            if found:
                return found
    raise PlanError(f"{path} has no robco_boards line")


def behind(repo, base, head):
    """Commits in head that base lacks, or "unknown" when git cannot tell."""
    try:
        return git(repo, "rev-list", "--count", f"{base}..{head}")
    except PlanError:
        return "unknown"


def main_ref(repo):
    for ref in ("origin/main", "main"):
        try:
            git(repo, "rev-parse", "--verify", "--quiet", f"{ref}^{{commit}}")
            return ref
        except PlanError:
            continue
    return None


def check_pins(repo_dir, core_path):
    script = os.path.join(core_path, "tools", "check_pins.sh")
    if not os.path.isfile(os.path.join(repo_dir, script)):
        raise PlanError(f"{repo_dir}/{script} is missing: is the submodule checked out?")
    proc = subprocess.run([os.path.join(".", script), ".github/workflows/ci.yml"],
                          cwd=repo_dir, capture_output=True, text=True)
    if proc.returncode != 0:
        out = (proc.stdout + proc.stderr).strip()
        raise PlanError(f"check_pins.sh failed: {out}")


def pinned_core(repo_dir, core_path):
    """The library commit the repository's own gitlink records. The gitlink
    lives in the repository that holds the submodule: Pip-OS inside Deitrix
    for Deitrix."""
    holder, _, leaf = core_path.rpartition("/")
    holder, _, _ = holder.rpartition("/")
    return git(os.path.join(repo_dir, holder), "rev-parse", f"HEAD:external/{leaf}")


def differs(what, pinned, built):
    if pinned == built:
        return None
    return f"{what}: pinned `{pinned[:12]}`, built with `{built[:12]}`"


def resolve(work, environ=None):
    """Read every checkout and return one record per built thing, by name.

    A record holds repo, ref, sha (the commit built), core (the library
    commit it builds with), pipos (the Pip-OS commit, Deitrix only), tag
    (the image it builds in), behind (library commits behind main) and
    notes, one per value that differs from the repository's own pin. The
    matrices, the summary and VERSIONS.txt all read these records and
    nothing else. Raises PlanError listing every problem."""
    environ = os.environ if environ is None else environ
    errors = []
    rows = {}
    core_dir = os.path.join(work, CORE)

    def record(name, sha, core, tag, pipos="", notes=()):
        ref = environ.get(f"NIGHTLY_REF_{REF_ENV[name]}", "") or "default branch"
        return {"repo": name, "ref": ref, "sha": sha, "core": core, "pipos": pipos,
                "tag": tag, "notes": [n for n in notes if n]}

    try:
        main = main_ref(core_dir)
        sha = git(core_dir, "rev-parse", "HEAD")
        rows[CORE] = record(CORE, sha, "", image_tag(core_dir))
        rows[CORE]["behind"] = behind(core_dir, "HEAD", main) if main else "unknown"
    except PlanError as e:
        errors.append(f"{CORE}: {e}")
        main = None
    pinned = {}
    for name, _, core_path in SIBLINGS:
        repo_dir = os.path.join(work, name)
        try:
            sha = git(repo_dir, "rev-parse", "HEAD")
            pin = pinned_core(repo_dir, core_path)
            tag = image_tag(os.path.join(repo_dir, core_path))
            check_pins(repo_dir, core_path)
            if name == "Deitrix":
                pinned["pipos"] = git(repo_dir, "rev-parse", "HEAD:external/pip-os")
        except PlanError as e:
            errors.append(f"{name}: {e}")
            continue
        pinned[name] = (pin, tag)
        rows[name] = record(name, sha, pin, tag)
    if errors:
        raise PlanError("\n".join(errors))
    # Deitrix builds on the Pip-OS head, so with the library and the image
    # tag of that head, not the ones its own older Pip-OS pin records.
    pipos = rows["Pip-OS"]
    pin, tag = pinned["Deitrix"]
    rows["Deitrix"] = record(
        "Deitrix", rows["Deitrix"]["sha"], pipos["core"], pipos["tag"], pipos["sha"],
        (differs("Pip-OS", pinned["pipos"], pipos["sha"]),
         differs("library", pin, pipos["core"]),
         differs("image tag", tag, pipos["tag"])))
    for name, _, _ in SIBLINGS:
        row = rows[name]
        row["behind"] = behind(core_dir, row["core"], main) if main else "unknown"
    gap = behind(os.path.join(work, "Pip-OS"), pinned["pipos"], pipos["sha"])
    plural = "" if gap == "1" else "s"
    note = (f"Deitrix pins Pip-OS `{pinned['pipos'][:12]}`, {gap} commit{plural} behind the "
            f"Pip-OS head `{pipos['sha'][:12]}`. The nightly builds Deitrix on the head.")
    return rows, note


def matrices(rows, work, pairing="pins"):
    """The six matrices of one pairing: pins or core-head."""
    def entry(repo):
        row = rows[repo]
        out = {"pairing": pairing, "repo": repo, "sha": row["sha"], "core_sha": row["core"],
               "pipos_sha": row["pipos"], "tag": row["tag"]}
        if pairing == "core-head":
            out["core_sha"], out["tag"] = rows[CORE]["sha"], rows[CORE]["tag"]
        return out

    def tested(repo):
        listed = boards(os.path.join(work, repo))
        if pairing == "core-head":
            return [b for b in listed if b in CORE_HEAD_BOARDS]
        return listed

    out = {
        "stages": [entry(r) for r in ("RETROS-BIOS", "MFBootAgent")],
        "card_terminal": [entry("UOS")],
        "card_pipboy": [entry("Deitrix")],
    }
    for key, repo in (("agent", "MFBootAgent"), ("uos", "UOS"), ("pipboy", "Deitrix")):
        out[key] = [dict(entry(repo), board=b) for b in tested(repo)]
    return {k: {"include": v} for k, v in out.items()}


def plan_pairings(rows, work, pairing):
    """The six matrices for pins, core-head or both, pins entries first."""
    names = ("pins", "core-head") if pairing == "both" else (pairing,)
    plan = {}
    for name in names:
        for key, value in matrices(rows, work, name).items():
            plan.setdefault(key, {"include": []})["include"] += value["include"]
    return plan, names


def core_head_line(plan):
    """The line that names the library and image of the core-head entries,
    or None when core-head is not planned."""
    for entry in plan.get("stages", {}).get("include", []):
        if entry["pairing"] == "core-head":
            return (f"In `core-head` every sibling builds on RobCo-Core "
                    f"`{entry['core_sha'][:12]}`, in the image `{entry['tag']}`. "
                    f"The Library and Image tag columns show the `pins` side.")
    return None


def table(rows, note, extra=None):
    lines = ["| Repository | Ref | Commit | Library | Pip-OS | Behind main | Image tag | Note |",
             "|---|---|---|---|---|---|---|---|"]

    def short(sha):
        return f"`{sha[:12]}`" if sha else "none"

    for name in (CORE,) + tuple(s[0] for s in SIBLINGS):
        row = rows[name]
        lines.append(f"| {name} | `{row['ref']}` | {short(row['sha'])} | {short(row['core'])} | "
                     f"{short(row['pipos'])} | {row['behind']} | `{row['tag']}` | "
                     f"{'. '.join(row['notes'])} |")
    lines += ["", note, ""]
    if extra:
        lines += [extra, ""]
    return "\n".join(lines)


def append(path, text):
    with open(path, "a", encoding="utf-8") as f:
        f.write(text)


def run(argv, environ=os.environ):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--work", required=True)
    p.add_argument("--pairing", required=True, choices=PAIRINGS)
    p.add_argument("--github-output", required=True)
    p.add_argument("--summary", required=True)
    p.add_argument("--versions")
    args = p.parse_args(argv)
    try:
        rows, note = resolve(args.work, environ)
        plan, names = plan_pairings(rows, args.work, args.pairing)
    except PlanError as e:
        for line in str(e).splitlines():
            print(f"::error::plan: {line}")
        return 1
    text = table(rows, note, core_head_line(plan))
    heading = ("## Nightly versions\n\nPairings planned: "
               + ", ".join(f"`{n}`" for n in names) + ".\n\n")
    append(args.summary, heading + text)
    if args.versions:
        with open(args.versions, "w", encoding="utf-8") as f:
            f.write(text)
    append(args.github_output, "".join(
        f"{k}={json.dumps(v, separators=(',', ':'))}\n" for k, v in plan.items()))
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(run(sys.argv[1:]))
