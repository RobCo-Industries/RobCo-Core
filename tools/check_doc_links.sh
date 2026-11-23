#!/usr/bin/env bash
# Checks that every RobCo-Core link in a project's Markdown files points at
# the commit its RobCo-Core submodule has checked out.
#
#   external/robco-core/tools/check_doc_links.sh <root> <submodule> [exclude...]
#
# root is the project root. submodule is the RobCo-Core submodule path,
# relative to root. Each exclude is one more path, relative to root, to skip.
# A link is github.com/RobCo-Industries/RobCo-Core/(blob|tree|commit)/<hash>,
# and its hash must equal the full submodule commit.
#
# Like check_pins.sh, it reads the commit from the submodule's git files and
# never calls git: in a CI container git refuses a checkout owned by another
# user. Without git it cannot tell tracked files, so it scans every *.md file
# under root. It skips CHANGELOG.md, which records old pins on purpose, and
# CLAUDE.md and AGENTS.md at any depth. It skips the submodule, the excludes
# and every directory named .git, .venv, .claude, build, node_modules,
# vendor or third_party.
# Exits 0 when every link matches, 1 when one differs or the commit is
# unknown, 2 on bad usage.
set -euo pipefail

die() {
    echo "check-doc-links: $*" >&2
    exit 1
}

[ $# -ge 2 ] || { echo "usage: $0 <root> <submodule> [exclude...]" >&2; exit 2; }
root=$1
core=${2%/}
shift 2
cd "$root" || die "cannot enter $root"
[ -e "$core" ] || die "$core is missing under $root: run git submodule update --init"

# The git directory of the submodule: .git is a directory, or a file with
# "gitdir: <path>", the path relative to the submodule.
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
[ -f "$gitdir/HEAD" ] || die "no HEAD in $gitdir: is $core a git submodule? run git submodule update --init"

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

prune=(-name .git -o -name .venv -o -name .claude -o -name build -o -name node_modules
    -o -name vendor -o -name third_party -o -path "./$core")
for x in "$@"; do
    prune+=(-o -path "./${x%/}")
done

status=0
while IFS= read -r -d '' f; do
    case "${f##*/}" in
    CHANGELOG.md | CLAUDE.md | AGENTS.md) continue ;;
    esac
    while IFS=: read -r n hash; do
        if [ "$hash" != "$commit" ]; then
            echo "check-doc-links: ${f#./}:$n links RobCo-Core at $hash, the submodule is at $commit" >&2
            status=1
        fi
    done < <(grep -noE 'github\.com/RobCo-Industries/RobCo-Core/(blob|tree|commit)/[0-9a-fA-F]+' "$f" |
        sed -E 's#^([0-9]+):.*/(blob|tree|commit)/#\1:#')
done < <(find . \( "${prune[@]}" \) -prune -o -name '*.md' -type f -print0)

[ "$status" -eq 0 ] && echo "check-doc-links: ok ($commit)"
exit "$status"
