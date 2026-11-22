#!/usr/bin/env bash
# Builds an SD card image file from a directory tree, with mtools and no root.
#
#   tools/mksdimage.sh <out.img> <size-mib> <dir>
#
# The image has an MBR with one FAT partition at LBA 2048: FAT16 (type
# 0x0E) below 64 MiB, FAT32 (type 0x0C) from 64 MiB. The size is rounded
# up to a power of 2, at least 4 MiB, because QEMU's SD card model needs
# that. Every file and directory under <dir> is copied to the partition
# root. tools/mkbootdir.sh assembles such a directory. The script writes
# only regular files: it refuses a device or a directory as <out.img>.
set -euo pipefail

PART_LBA=2048
SECTOR=512
FAT32_MIN_MIB=64

die() {
    echo "mksdimage: $*" >&2
    exit 1
}

usage() {
    echo "usage: $0 <out.img> <size-mib> <dir>" >&2
    exit 2
}

[ $# -eq 3 ] || usage
out=$1
size_mib=$2
dir=$3

case "$size_mib" in
''|*[!0-9]*) die "size '$size_mib' is not a number of MiB" ;;
esac
[ "$size_mib" -gt 0 ] || die "size must be above 0"
[ "$size_mib" -le 32768 ] || die "size above 32768 MiB"

case "$out" in
/dev/*) die "refusing to write to $out: a device path" ;;
esac
if [ -b "$out" ] || [ -c "$out" ]; then
    die "refusing to write to $out: a device"
fi
[ ! -d "$out" ] || die "$out is a directory"
[ -d "$dir" ] || die "$dir is not a directory"

for t in mformat mcopy; do
    command -v "$t" >/dev/null 2>&1 || die "$t not found: install mtools"
done

mib=4
while [ "$mib" -lt "$size_mib" ]; do
    mib=$((mib * 2))
done
bytes=$((mib * 1024 * 1024))
sectors=$((bytes / SECTOR - PART_LBA))
if [ "$mib" -ge "$FAT32_MIN_MIB" ]; then
    ptype=0c
    fmt=(-F)
else
    ptype=0e
    # One-sector clusters keep small cards above the FAT12 cluster limit.
    fmt=(-c 1)
fi

# Four little-endian bytes of $1 as printf escapes.
le32() {
    printf '\\x%02x\\x%02x\\x%02x\\x%02x' $(($1 & 255)) $(($1 >> 8 & 255)) \
        $(($1 >> 16 & 255)) $(($1 >> 24 & 255))
}

tmp="$out.tmp.$$"
trap 'rm -f "$tmp" "$tmp.mbr"' EXIT

# Sparse file of the full size.
: >"$tmp"
dd if=/dev/zero of="$tmp" bs=1 count=0 seek="$bytes" 2>/dev/null

# MBR: one entry, CHS fields set to the LBA-only marker values.
{
    head -c 446 /dev/zero
    # shellcheck disable=SC2059
    printf "\\x00\\xfe\\xff\\xff\\x$ptype\\xfe\\xff\\xff$(le32 "$PART_LBA")$(le32 "$sectors")"
    head -c 48 /dev/zero
    printf '\x55\xaa'
} >"$tmp.mbr"
[ "$(wc -c <"$tmp.mbr" | tr -d ' ')" -eq 512 ] || die "internal error: MBR is not 512 bytes"
dd if="$tmp.mbr" of="$tmp" bs=512 count=1 conv=notrunc 2>/dev/null

export MTOOLS_SKIP_CHECK=1
img="$tmp@@$((PART_LBA * SECTOR))"
mformat -i "$img" "${fmt[@]}" -v ROBCO -T "$sectors" -h 64 -s 32 -H "$PART_LBA" :: \
    || die "mformat failed"

# Top-level entries, hidden ones included. mcopy -s copies directories recursively.
while IFS= read -r -d '' entry; do
    mcopy -s -p -m -Q -i "$img" "$entry" :: >/dev/null || die "mcopy failed for $entry"
done < <(find "$dir" -mindepth 1 -maxdepth 1 -print0)

mv -f "$tmp" "$out"
rm -f "$tmp.mbr"
trap - EXIT
echo "mksdimage: $out ($mib MiB, FAT$([ "$ptype" = 0c ] && echo 32 || echo 16))"
