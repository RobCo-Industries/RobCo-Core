#!/usr/bin/env python3
"""Plan one nightly run: resolve the commits, the image tags and the matrices.

    plan.py --work w --pairing pins --github-output F --summary F [--versions F]

--work holds one checkout per repository, named as the repository:
w/RobCo-Core and the five siblings, each sibling with its submodules.
The script reads each checkout with git and runs each sibling's own
tools/check_pins.sh on its .github/workflows/ci.yml, which proves that
the image tag in the sibling's CI equals the ci/IMAGE_TAG the nightly
reads. A mismatch, a missing file or a failed git command fails the plan.

It appends six JSON matrices to --github-output: stages, card_terminal,
card_pipboy, agent, uos and pipboy. Each entry holds pairing, repo, sha,
core_sha, pipos_sha and tag, and board in the three test matrices. It
appends the version table to --summary and writes it to --versions.

Only the pins pairing exists. "both" plans pins alone. "core-head" fails.

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


def resolve(work):
    """Read every checkout. Returns (rows, deitrix_note) or raises PlanError
    listing every problem."""
    errors = []
    rows = {}
    core_dir = os.path.join(work, CORE)
    try:
        main = main_ref(core_dir)
        rows[CORE] = {
            "repo": CORE,
            "sha": git(core_dir, "rev-parse", "HEAD"),
            "pin": "",
            "behind": behind(core_dir, "HEAD", main) if main else "unknown",
            "tag": image_tag(core_dir),
        }
    except PlanError as e:
        errors.append(f"{CORE}: {e}")
        main = None
    for name, _, core_path in SIBLINGS:
        repo_dir = os.path.join(work, name)
        try:
            sha = git(repo_dir, "rev-parse", "HEAD")
            # The gitlink lives in the repository that holds the submodule:
            # Pip-OS inside Deitrix for Deitrix.
            holder, _, leaf = core_path.rpartition("/")
            holder, _, _ = holder.rpartition("/")
            pin = git(os.path.join(repo_dir, holder), "rev-parse", f"HEAD:external/{leaf}")
            tag = image_tag(os.path.join(repo_dir, core_path))
            check_pins(repo_dir, core_path)
        except PlanError as e:
            errors.append(f"{name}: {e}")
            continue
        rows[name] = {
            "repo": name, "sha": sha, "pin": pin, "tag": tag,
            "behind": behind(core_dir, pin, main) if main else "unknown",
        }
    if errors:
        raise PlanError("\n".join(errors))
    deitrix_dir = os.path.join(work, "Deitrix")
    pipos_pin = git(deitrix_dir, "rev-parse", "HEAD:external/pip-os")
    pipos_head = rows["Pip-OS"]["sha"]
    gap = behind(os.path.join(work, "Pip-OS"), pipos_pin, pipos_head)
    plural = "" if gap == "1" else "s"
    note = (f"Deitrix pins Pip-OS `{pipos_pin[:12]}`, {gap} commit{plural} behind the "
            f"Pip-OS head `{pipos_head[:12]}`. The nightly builds Deitrix on the head.")
    return rows, note


def matrices(rows, work, pairing="pins"):
    def entry(repo, sha, pipos="", core_row=None):
        core_row = core_row or rows[repo]
        return {"pairing": pairing, "repo": repo, "sha": sha, "core_sha": "",
                "pipos_sha": pipos, "tag": core_row["tag"]}

    pipos = rows["Pip-OS"]
    # Deitrix builds on the Pip-OS head and the RobCo-Core inside it.
    deitrix = entry("Deitrix", rows["Deitrix"]["sha"], pipos["sha"], pipos)
    out = {
        "stages": [entry(r, rows[r]["sha"]) for r in ("RETROS-BIOS", "MFBootAgent")],
        "card_terminal": [entry("UOS", rows["UOS"]["sha"])],
        "card_pipboy": [deitrix],
    }
    for key, repo in (("agent", "MFBootAgent"), ("uos", "UOS"), ("pipboy", "Deitrix")):
        base = deitrix if repo == "Deitrix" else entry(repo, rows[repo]["sha"])
        out[key] = [dict(base, board=b) for b in boards(os.path.join(work, repo))]
    return {k: {"include": v} for k, v in out.items()}


def table(rows, note, environ):
    lines = ["| Repository | Ref | Commit | Library pin | Behind main | Image tag |",
             "|---|---|---|---|---|---|"]
    for name in (CORE,) + tuple(s[0] for s in SIBLINGS):
        row = rows[name]
        ref = environ.get(f"NIGHTLY_REF_{REF_ENV[name]}", "") or "default branch"
        pin = f"`{row['pin'][:12]}`" if row["pin"] else "none"
        lines.append(f"| {name} | `{ref}` | `{row['sha'][:12]}` | {pin} | "
                     f"{row['behind']} | `{row['tag']}` |")
    lines += ["", note, ""]
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
    if args.pairing == "core-head":
        print("::error::plan: the core-head pairing is not built yet. Use pins or both.")
        return 1
    try:
        rows, note = resolve(args.work)
        plan = matrices(rows, args.work)
    except PlanError as e:
        for line in str(e).splitlines():
            print(f"::error::plan: {line}")
        return 1
    text = table(rows, note, environ)
    heading = "## Nightly versions\n\n"
    if args.pairing == "both":
        heading += "Pairing `both` runs `pins` only: `core-head` is not built yet.\n\n"
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
