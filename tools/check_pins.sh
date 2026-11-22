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
# scan needs python3, standard library only. It is not a YAML parser:
# it reads block mappings and with: as a flow mapping on one line, such
# as with: {image-tag: abc, x: "y"}, with quoted or bare values. Under
# jobs:, it stops with an error naming the line on what it cannot read:
# tabs in the indentation, anchors and aliases, <<: merge keys, block
# scalars in uses: or image-tag, a flow mapping over several lines, or
# with: given through an expression.
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
set +e
calls=$(python3 - "$workflow" <<'PY'
import re
import sys

USES = re.compile(r"[^/\s]+/RobCo-Core/\.github/workflows/robco-ci\.yml@(\S*)$")
KEY = re.compile(r"([A-Za-z0-9_.<-]+|\"[^\"]*\"|'[^']*')\s*:(\s|$)(.*)$")


class Unsupported(Exception):
    pass


def strip_comment(text):
    """The text before a " #" comment that is outside quotes."""
    quote = None
    for i, c in enumerate(text):
        if quote:
            if c == quote:
                quote = None
        elif c in "'\"":
            quote = c
        elif c == "#" and (i == 0 or text[i - 1] in " \t"):
            return text[:i].strip()
    return text.strip()


def value(text):
    """A scalar without its quotes or its trailing comment."""
    text = strip_comment(text)
    if text[:1] in ("'", '"'):
        if len(text) < 2 or text[-1] != text[0]:
            raise Unsupported("an unclosed quoted value")
        return text[1:-1]
    if text[:1] in ("&", "*"):
        raise Unsupported("an anchor or alias")
    if text[:1] in ("|", ">"):
        raise Unsupported("a block scalar")
    if text[:1] in ("{", "["):
        raise Unsupported("a nested flow collection")
    return text


def flow_mapping(text):
    """{key: value, ...} on one line, scalar values only."""
    text = strip_comment(text)
    if not text.endswith("}"):
        raise Unsupported("a flow mapping that spans several lines")
    parts, quote, cur = [], None, ""
    for c in text[1:-1]:
        if quote:
            if c == quote:
                quote = None
        elif c in "'\"":
            quote = c
        elif c in "{}[]":
            raise Unsupported("a nested flow collection")
        elif c == ",":
            parts.append(cur)
            cur = ""
            continue
        cur += c
    if quote:
        raise Unsupported("an unclosed quoted value")
    parts.append(cur)
    out = {}
    for part in parts:
        if not part.strip():
            continue
        m = KEY.match(part.strip())
        if not m:
            raise Unsupported("a flow mapping entry that is not key: value")
        out[m.group(1).strip("\"'")] = value(m.group(3))
    return out


def entries(lines):
    """(line number, indent, key, rest) for each line, blank and comment lines skipped."""
    for n, raw in enumerate(lines, 1):
        line = raw.rstrip("\r\n").rstrip()
        stripped = line.lstrip(" ")
        if not stripped or stripped.startswith("#"):
            continue
        if stripped[0] == "\t":
            yield n, None, "\t", ""
            continue
        m = KEY.match(stripped)
        if m:
            yield n, len(line) - len(stripped), m.group(1).strip("\"'"), m.group(3)
        else:
            yield n, len(line) - len(stripped), None, stripped


def scan(items):
    in_jobs = False
    job_indent = None
    job = None
    with_indent = None
    calls = {}
    for n, indent, key, rest in items:
        if not in_jobs:
            in_jobs = indent == 0 and key == "jobs"
            continue
        if indent == 0:
            break
        try:
            if key == "\t":
                raise Unsupported("a tab in the indentation")
            if key == "<<":
                raise Unsupported("a <<: merge key")
            r = strip_comment(rest)
            if r[:1] in ("&", "*") or key is None and re.match(r"-?\s*[&*]", r):
                raise Unsupported("an anchor or alias")
            if job_indent is None:
                job_indent = indent
            if indent <= job_indent:
                if r:
                    raise Unsupported("a job that is not a block mapping")
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
            elif key == "with":
                if not r:
                    with_indent = indent
                elif r.startswith("{"):
                    tag = flow_mapping(rest).get("image-tag")
                    if tag is not None:
                        calls.setdefault(job, [None, None])[1] = tag
                elif r.startswith("${{"):
                    raise Unsupported("a with: given through an expression")
                else:
                    value(rest)
                    raise Unsupported("a with: that is not a mapping")
        except Unsupported as e:
            sys.exit(f"check-pins: {sys.argv[1]}:{n}: cannot read {e}; "
                     "this script supports block mappings and single-line "
                     "flow mappings only")
    return calls


with open(sys.argv[1], encoding="utf-8") as f:
    calls = scan(entries(f))
for name, (uses, tag) in calls.items():
    if uses is not None:
        print(name, uses or "-", tag or "-")
PY
)
rc=$?
set -e
# The scan prints its own message when it cannot read the workflow.
[ "$rc" -eq 0 ] || exit 1

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
