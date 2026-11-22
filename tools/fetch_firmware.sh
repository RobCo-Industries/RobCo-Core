#!/usr/bin/env bash
# Downloads the Raspberry Pi boot firmware at one pinned commit and checks it.
#
#   tools/fetch_firmware.sh <target-dir>          download, verify, install
#   tools/fetch_firmware.sh --check <target-dir>  verify files already there
#
# Writes only bootcode.bin, start.elf and fixup.dat, and only inside
# <target-dir>. Needs curl and sha256sum or shasum. Does not need root.
# FIRMWARE_URL overrides the download base, for tests and mirrors.
set -euo pipefail

# raspberrypi/firmware tag 1.20260915.
FIRMWARE_TAG="1.20260915"
FIRMWARE_COMMIT="6f0881cba8bea8ec24956a5718714730e9936ec5"
FIRMWARE_URL="${FIRMWARE_URL:-https://raw.githubusercontent.com/raspberrypi/firmware/${FIRMWARE_COMMIT}/boot}"

FILES=(bootcode.bin start.elf fixup.dat)

expected_sha() {
    case "$1" in
    bootcode.bin) echo 4245ec23b58158dc3e55f3e065eb4ecc0c0705d76c8db506e9f824941feee32e ;;
    start.elf)    echo 32f990801e7765bae6fe5cb940ba5eeff02dd0dac8dc5474db5b3fd345281790 ;;
    fixup.dat)    echo 7853f2b51e4489f8b3ce6f9c52fd0afdda38c7383961524e0883d34afcb4827b ;;
    *) return 1 ;;
    esac
}

die() {
    echo "fetch_firmware: $*" >&2
    exit 1
}

usage() {
    echo "usage: $0 [--check] <target-dir>" >&2
    exit 2
}

sha256() {
    if command -v sha256sum > /dev/null; then
        sha256sum "$1" | cut -d' ' -f1
    elif command -v shasum > /dev/null; then
        shasum -a 256 "$1" | cut -d' ' -f1
    else
        die "needs sha256sum or shasum"
    fi
}

# Fails unless every firmware file in $1 is a regular file with the pinned hash.
verify_dir() {
    local dir=$1 f got bad=0

    for f in "${FILES[@]}"; do
        if [ -L "$dir/$f" ] || [ ! -f "$dir/$f" ]; then
            echo "fetch_firmware: $dir/$f: missing or not a regular file" >&2
            bad=1
            continue
        fi
        got=$(sha256 "$dir/$f")
        if [ "$got" != "$(expected_sha "$f")" ]; then
            echo "fetch_firmware: $dir/$f: SHA-256 $got does not match the pinned value" >&2
            bad=1
        fi
    done
    return "$bad"
}

check=0
target=""
while [ $# -gt 0 ]; do
    case "$1" in
    --check) check=1 ;;
    -h|--help) usage ;;
    -*) echo "fetch_firmware: unknown option $1" >&2; usage ;;
    *)
        [ -z "$target" ] || usage
        target=$1
        ;;
    esac
    shift
done
[ -n "$target" ] || usage

if [ "$check" -eq 1 ]; then
    [ -d "$target" ] || die "$target is not a directory"
    verify_dir "$target" || die "check failed for $target"
    echo "fetch_firmware: $target matches tag $FIRMWARE_TAG"
    exit 0
fi

case "$target" in
/dev|/dev/*) die "refusing to write to $target" ;;
esac
if [ -e "$target" ] && [ ! -d "$target" ]; then
    die "$target exists and is not a directory"
fi
mkdir -p -- "$target"
for f in "${FILES[@]}"; do
    # A symlink or a directory here would send the write somewhere else.
    if [ -L "$target/$f" ] || { [ -e "$target/$f" ] && [ ! -f "$target/$f" ]; }; then
        die "refusing to replace $target/$f: not a regular file"
    fi
done
command -v curl > /dev/null || die "needs curl"

tmp=$(mktemp -d "${TMPDIR:-/tmp}/fetch_firmware.XXXXXX")
trap 'rm -rf -- "$tmp"' EXIT

for f in "${FILES[@]}"; do
    curl -fsSL --proto '=https,file' -o "$tmp/$f" "$FIRMWARE_URL/$f" || die "download of $f failed"
done
verify_dir "$tmp" || die "downloaded files do not match the pinned hashes, nothing written"
for f in "${FILES[@]}"; do
    cp -- "$tmp/$f" "$target/$f"
done
echo "fetch_firmware: wrote ${FILES[*]} from tag $FIRMWARE_TAG to $target"
