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

# The values in the workflow. Quotes and trailing comments are allowed.
uses=$(sed -n 's/^[[:space:]]*-\{0,1\}[[:space:]]*uses:[[:space:]]*["'\'']\{0,1\}[^/[:space:]]*\/RobCo-Core\/\.github\/workflows\/robco-ci\.yml@\([^"'\''[:space:]]*\).*$/\1/p' "$workflow")
wtag=$(sed -n 's/^[[:space:]]*image-tag:[[:space:]]*["'\'']\{0,1\}\([0-9A-Za-z._-]*\).*$/\1/p' "$workflow")

status=0
[ -n "$uses" ] || { echo "check-pins: $workflow does not call RobCo-Core/.github/workflows/robco-ci.yml" >&2; status=1; }
for u in $uses; do
    if [ "$u" != "$commit" ]; then
        echo "check-pins: $workflow uses robco-ci.yml@$u, the submodule is at $commit" >&2
        status=1
    fi
done
[ -n "$wtag" ] || { echo "check-pins: $workflow sets no image-tag" >&2; status=1; }
for t in $wtag; do
    if [ "$t" != "$tag" ]; then
        echo "check-pins: $workflow uses image-tag $t, $core/ci/IMAGE_TAG is $tag" >&2
        status=1
    fi
done
[ "$status" -eq 0 ] && echo "check-pins: ok ($commit, $tag)"
exit "$status"
