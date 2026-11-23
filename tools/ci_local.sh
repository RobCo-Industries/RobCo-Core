#!/bin/sh
# Run a CI build in the robco-ci container, as GitHub Actions does:
#   make CI=1 <targets>
# The tree is copied to build/ci-local/src, so the container's outputs
# never mix with the host's. The container runs as the calling user with
# HOME=/tmp, no host environment and at most CI_LOCAL_CPUS CPUs.
#
# Usage: tools/ci_local.sh [--root DIR] [make arguments]
#        tools/ci_local.sh [--root DIR] --build-image
#   --root DIR     the tree to copy. Default: the current directory.
#   --build-image  build robco-ci:<tag> from ci/Dockerfile and exit.
#   make arguments default to CI_LOCAL_TARGETS, else "all test-host".
#
# Environment, read by this script only:
#   CI_IMAGE        the image to run. Default: robco-ci:<tag> when it
#                   exists, else ghcr.io/robco-industries/robco-ci:<tag>.
#                   <tag> comes from ci/IMAGE_TAG next to this script.
#   CI_LOCAL_CPUS   docker run --cpus. Default: 4.
#   CI_LOCAL_TARGETS  default make arguments, split on spaces.
set -eu

die() { echo "ci-local: $*" >&2; exit 1; }

core=$(cd "$(dirname "$0")/.." && pwd)
root=$(pwd)
build_image=0
while [ $# -gt 0 ]; do
    case "$1" in
        --root) [ $# -ge 2 ] || die "--root needs a directory"
                root=$(cd "$2" 2>/dev/null && pwd) || die "no directory $2"
                shift 2 ;;
        --build-image) build_image=1; shift ;;
        --) shift; break ;;
        *) break ;;
    esac
done

[ -f "$core/ci/IMAGE_TAG" ] || die "no $core/ci/IMAGE_TAG"
tag=$(tr -d '[:space:]' < "$core/ci/IMAGE_TAG")
local_image="robco-ci:$tag"
published="ghcr.io/robco-industries/robco-ci:$tag"
cpus=${CI_LOCAL_CPUS:-4}
case "$cpus" in
    ''|*[!0-9.]*) die "CI_LOCAL_CPUS must be a number, got '$cpus'" ;;
esac

if [ $# -eq 0 ]; then
    # Split on spaces on purpose: CI passes make-targets the same way.
    # shellcheck disable=SC2086
    set -- ${CI_LOCAL_TARGETS:-all test-host}
fi
# A bare -j starts unlimited jobs; the CPU limit does not bound load.
for a in "$@"; do
    case "$a" in
        -j|--jobs) die "give -j a number, for example -j$cpus" ;;
    esac
done

command -v docker >/dev/null 2>&1 || die "docker not found. Install Docker to run ci-local."
docker info >/dev/null 2>&1 || die "Docker is not running. Start it and try again."

if [ "$build_image" = 1 ]; then
    echo "ci-local: building $local_image from $core/ci/Dockerfile"
    exec docker build -f "$core/ci/Dockerfile" -t "$local_image" "$core/ci"
fi

if [ -n "${CI_IMAGE:-}" ]; then
    image=$CI_IMAGE
    docker image inspect "$image" >/dev/null 2>&1 || docker pull -q "$image" >/dev/null 2>&1 \
        || die "image $image not found"
elif docker image inspect "$local_image" >/dev/null 2>&1; then
    image=$local_image
elif docker image inspect "$published" >/dev/null 2>&1 \
        || docker pull -q "$published" >/dev/null 2>&1; then
    image=$published
else
    die "no image $local_image or $published. Build it with: make ci-local-image"
fi

work="$root/build/ci-local"
mkdir -p "$work"
chmod -R u+w "$work/src" 2>/dev/null || true
rm -rf "$work/src"
mkdir -p "$work/src"
# tar, not rsync: the robco-ci image has no rsync for these tests.
(cd "$root" && tar -cf - --exclude ./build --exclude ./.venv --exclude .git .) \
    | (cd "$work/src" && tar -xf -)

# The doc-link check reads HEAD in each checkout. Give the tree, and the
# library when it sits inside the tree, a minimal .git with its commit,
# as actions/checkout leaves.
head_of() {
    git -C "$1" rev-parse HEAD 2>/dev/null || echo 0000000000000000000000000000000000000000
}
mkdir -p "$work/src/.git"
head_of "$root" > "$work/src/.git/HEAD"
case "$core" in
    "$root"/*)
        rel=${core#"$root"/}
        mkdir -p "$work/src/$rel/.git"
        head_of "$core" > "$work/src/$rel/.git/HEAD" ;;
esac

echo "ci-local: $image, $cpus CPUs: make CI=1 $*"
exec docker run --rm --cpus "$cpus" --user "$(id -u):$(id -g)" -e HOME=/tmp \
    -v "$work/src:/work" -w /work "$image" make CI=1 "$@"
