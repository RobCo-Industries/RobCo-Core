#!/usr/bin/env bash
# Checks that a project's workflow calls robco-ci.yml at the commit its
# RobCo-Core submodule has checked out, with the image tag from the
# submodule's ci/IMAGE_TAG. A reusable workflow takes literals only, so
# these values are copies and can drift.
#
#   external/robco-core/tools/check_pins.sh .github/workflows/ci.yml
#
# Run it from the project root. The submodule is the directory above this
# script, or ROBCO_CORE if set. The commit comes from the submodule's git
# files, not from git: in a CI container git refuses a checkout owned by
# another user. When git works, it also checks that the checkout matches
# the commit the project records, and warns if not.
# It reads the workflow per job: each job that calls robco-ci.yml must
# carry both pins, in its own uses: and its own with: image-tag. The
# scan needs python3, standard library only.
# Exits 0 when both pins match, 1 when one differs, 2 on bad usage.
set -euo pipefail

die() {
    echo "check-pins: $*" >&2
    exit 1
}

[ $# -eq 1 ] || { echo "usage: $0 <workflow-file>" >&2; exit 2; }
workflow=$1
[ -f "$workflow" ] || die "$workflow is missing"

core=${ROBCO_CORE:-$(cd "$(dirname "$0")/.." && pwd)}
here=$(pwd)
core=${core#"$here"/}
[ -f "$core/ci/IMAGE_TAG" ] || die "$core/ci/IMAGE_TAG is missing: run git submodule update --init"

# The git directory of the submodule: .git is a directory, or a file
# with "gitdir: <path>", the path relative to the submodule.
gitdir=$core/.git
if [ -f "$gitdir" ]; then
    line=$(head -n 1 "$gitdir")
    case "$line" in
    gitdir:*) ;;
    *) die "$gitdir does not start with gitdir:" ;;
    esac
    gitdir=${line#gitdir:}
    gitdir=${gitdir#"${gitdir%%[![:space:]]*}"}
    case "$gitdir" in
    /*) ;;
    *) gitdir=$core/$gitdir ;;
    esac
fi
[ -f "$gitdir/HEAD" ] || die "no HEAD in $gitdir: is $core a git checkout?"

# HEAD holds the commit when detached, or "ref: <name>" to resolve
# through refs/ or packed-refs.
head=$(tr -d '\r' < "$gitdir/HEAD" | head -n 1)
case "$head" in
ref:*)
    ref=${head#ref:}
    ref=${ref#"${ref%%[![:space:]]*}"}
    commit=
    # A linked work tree keeps shared refs in its common directory.
    common=$gitdir
    [ -f "$gitdir/commondir" ] && common=$gitdir/$(head -n 1 "$gitdir/commondir")
    for d in "$gitdir" "$common"; do
        if [ -f "$d/$ref" ]; then
            commit=$(head -n 1 "$d/$ref" | tr -d '\r')
            break
        fi
        if [ -f "$d/packed-refs" ]; then
            commit=$(awk -v r="$ref" '$2 == r { print $1; exit }' "$d/packed-refs")
            [ -n "$commit" ] && break
        fi
    done
    [ -n "$commit" ] || die "cannot resolve $ref in $gitdir"
    ;;
*) commit=$head ;;
esac
case "$commit" in
*[!0-9a-f]* | '') die "bad commit '$commit' in $gitdir" ;;
esac

tag=$(tr -d ' \t\r\n' < "$core/ci/IMAGE_TAG")

# Optional cross-check: the commit the project records for the submodule.
if recorded=$(git ls-files -s -- "$core" 2>/dev/null | awk '$1 == "160000" { print $2; exit }') &&
    [ -n "$recorded" ] && [ "$recorded" != "$commit" ]; then
    echo "check-pins: warning: $core is checked out at $commit, the project records $recorded" >&2
fi

# The values in the workflow, read per job by the python3 scan below.
# It prints one line per job that calls robco-ci.yml:
# "<job> <commit> <image-tag or ->".
command -v python3 >/dev/null 2>&1 || die "python3 is required to read $workflow"
calls=$(python3 - "$workflow" <<'PY'
import re
import sys

USES = re.compile(r"[^/\s]+/RobCo-Core/\.github/workflows/robco-ci\.yml@(\S*)$")


def value(text):
    """A scalar without its quotes or its trailing comment."""
    text = text.strip()
    if text[:1] in ("'", '"'):
        end = text.find(text[0], 1)
        return text[1:end] if end > 0 else text[1:]
    return re.split(r"\s#", text, maxsplit=1)[0].strip()


def entries(lines):
    """(indent, key, rest) for each line, blank and comment lines skipped."""
    for raw in lines:
        line = raw.rstrip("\r\n").rstrip()
        stripped = line.lstrip(" ")
        if not stripped or stripped.startswith("#"):
            continue
        m = re.match(r"([A-Za-z0-9_.-]+|\"[^\"]*\"|'[^']*'):(\s|$)(.*)$", stripped)
        if m:
            yield len(line) - len(stripped), m.group(1).strip("\"'"), m.group(3)
        else:
            yield len(line) - len(stripped), None, ""


with open(sys.argv[1], encoding="utf-8") as f:
    items = list(entries(f))

in_jobs = False
job_indent = None
job = None
with_indent = None
calls = {}
for indent, key, rest in items:
    if not in_jobs:
        in_jobs = indent == 0 and key == "jobs"
        continue
    if indent == 0:
        break
    if job_indent is None:
        job_indent = indent
    if indent <= job_indent:
        job, with_indent = key, None
        continue
    if with_indent is not None and indent > with_indent:
        if key == "image-tag":
            calls.setdefault(job, [None, None])[1] = value(rest)
        continue
    with_indent = None
    if key == "uses":
        m = USES.match(value(rest))
        if m:
            calls.setdefault(job, [None, None])[0] = m.group(1)
    elif key == "with" and not value(rest):
        with_indent = indent
for name, (uses, tag) in calls.items():
    if uses is not None:
        print(name, uses or "-", tag or "-")
PY
) || die "cannot read $workflow"

status=0
[ -n "$calls" ] || { echo "check-pins: $workflow has no job that calls RobCo-Core/.github/workflows/robco-ci.yml" >&2; status=1; }
while read -r job u t; do
    [ -n "$job" ] || continue
    if [ "$u" != "$commit" ]; then
        echo "check-pins: $workflow job $job uses robco-ci.yml@$u, the submodule is at $commit" >&2
        status=1
    fi
    if [ "$t" = "-" ]; then
        echo "check-pins: $workflow job $job sets no image-tag" >&2
        status=1
    elif [ "$t" != "$tag" ]; then
        echo "check-pins: $workflow job $job uses image-tag $t, $core/ci/IMAGE_TAG is $tag" >&2
        status=1
    fi
done <<EOF
$calls
EOF
[ "$status" -eq 0 ] && echo "check-pins: ok ($commit, $tag)"
exit "$status"
