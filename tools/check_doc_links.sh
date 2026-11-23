#!/usr/bin/env bash
# Checks that every link to a pinned repository in a project's Markdown files
# points at the commit its submodule has checked out.
#
#   external/robco-core/tools/check_doc_links.sh [--repo owner/name] <root> <submodule> [exclude...]
#
# --repo names the GitHub repository the links point at. The default is
# RobCo-Industries/RobCo-Core. root is the project root. submodule is the
# path of that repository's submodule, relative to root. It may be nested,
# such as external/pip-os/external/robco-core.
# One run checks one repository. A project that pins two repositories calls
# the script once per repository:
#
#   check_doc_links.sh --repo RobCo-Industries/Pip-OS . external/pip-os
#   check_doc_links.sh . external/pip-os/external/robco-core
#
# Each exclude is one more path, relative to root, to skip.
# Leading ./, doubled slashes and a trailing / are fine. An absolute path or
# a .. part in the submodule or an exclude is a usage error.
# It reads every github.com/<owner>/<name>/(blob|tree|commit)/<ref>
# URL, on any line, code blocks and reference definitions included. The ref
# ends at /, ), #, ?, whitespace, a quote, < or >, and at the characters that
# can follow a URL in Markdown or prose: ` ] } , ; : * | and \. Git forbids
# : * and \ in a ref name; a name with one of the others is not supported
# and is read only up to that character. A ref of 40 hex digits followed
# only by . or _ is that hash at a sentence end or inside _emphasis_. Any
# other . or _ is part of a branch or tag name, such as v1.0 or my_branch. A ref of exactly 40 hex
# digits is a pin and must equal the submodule commit; case does not matter.
# A ref of 7 to 39 hex digits is an abbreviated commit and is an error: use
# the full hash. Any other ref is a branch or tag name and is ignored.
#
# Like check_pins.sh, it reads the commit from the submodule's git files and
# never calls git: in a CI container git refuses a checkout owned by another
# user. Without git it cannot tell tracked files, so it scans every *.md file
# under root. It skips CHANGELOG.md, which records old pins on purpose, and
# CLAUDE.md and AGENTS.md at any depth. It skips the submodule, every
# directory above it that holds a .git entry (the outer submodule of a nested
# one, never root itself), the excludes
# and every directory named .git, .venv, .claude, build, node_modules,
# vendor or third_party.
# Exits 0 when every link matches, 1 when one differs or the commit is
# unknown, 2 on bad usage.
set -euo pipefail

die() {
    echo "check-doc-links: $*" >&2
    exit 1
}

usage="usage: $0 [--repo owner/name] <root> <submodule> [exclude...]"
repo=RobCo-Industries/RobCo-Core
if [ "${1-}" = --repo ]; then
    [ $# -ge 2 ] || { echo "$usage" >&2; exit 2; }
    repo=$2
    shift 2
fi
[[ $repo =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$ ]] || { echo "check-doc-links: bad --repo '$repo'" >&2; echo "$usage" >&2; exit 2; }
name=${repo#*/}
repo_re=${repo//./\\.}
[ $# -ge 2 ] || { echo "$usage" >&2; exit 2; }
# Normalizes a path relative to root: drops leading ./, doubled and trailing
# slashes. Refuses an absolute path, a .. part, or the root itself, because
# find reports paths as ./<rel> and could never match those.
rel() {
    local p=$1 q
    case "$p" in
    /*) bad "$2 '$1' must be relative to the root" ;;
    esac
    while :; do
        q=${p//\/\//\/}
        q=${q#./}
        q=${q%/}
        q=${q%/.}
        q=${q//\/.\//\/}
        [ "$q" = "$p" ] && break
        p=$q
    done
    case "/$p/" in
    //|/./) bad "$2 '$1' names the root itself" ;;
    */../*) bad "$2 '$1' must stay under the root" ;;
    esac
    printf '%s\n' "$p"
}
bad() {
    echo "check-doc-links: $*" >&2
    echo "$usage" >&2
    exit 2
}

root=$1
core=$(rel "$2" submodule) || exit 2
shift 2
excludes=()
for x in "$@"; do
    excludes+=("$(rel "$x" exclude)") || exit 2
done
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
# An outer submodule holds the inner one: skip each parent with a .git entry.
p=$core
while [ "${p%/*}" != "$p" ]; do
    p=${p%/*}
    [ -e "$p/.git" ] && prune+=(-o -path "./$p")
done
for x in ${excludes[@]+"${excludes[@]}"}; do
    prune+=(-o -path "./$x")
done

# A bracket expression: ] comes first to be literal, \ is literal inside.
ref_chars='[^]/)#?[:space:]"'"'"'<>`},;:*|\]*'
status=0
while IFS= read -r -d '' f; do
    case "${f##*/}" in
    CHANGELOG.md | CLAUDE.md | AGENTS.md) continue ;;
    esac
    while IFS=: read -r n seg; do
        if [[ $seg =~ ^([0-9a-fA-F]{40})[._]+$ ]]; then
            seg=${BASH_REMATCH[1]}
        fi
        hash=$(printf '%s' "$seg" | tr 'A-F' 'a-f')
        case "$hash" in
        '' | *[!0-9a-f]*) continue ;;
        esac
        if [ ${#hash} -eq 40 ]; then
            if [ "$hash" != "$commit" ]; then
                echo "check-doc-links: ${f#./}:$n links $name at $seg, the submodule is at $commit" >&2
                status=1
            fi
        elif [ ${#hash} -ge 7 ] && [ ${#hash} -lt 40 ]; then
            echo "check-doc-links: ${f#./}:$n links $name at abbreviated commit $seg, use the full hash $commit" >&2
            status=1
        fi
    done < <(grep -noE "github\.com/$repo_re/(blob|tree|commit)/$ref_chars" "$f" |
        sed -E 's#^([0-9]+):.*/(blob|tree|commit)/#\1:#')
done < <(find . \( "${prune[@]}" \) -prune -o -name '*.md' -type f -print0)

[ "$status" -eq 0 ] && echo "check-doc-links: ok ($commit)"
exit "$status"
