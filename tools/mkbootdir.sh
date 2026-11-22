#!/usr/bin/env bash
# Assembles the files for the FAT boot partition of a real SD card.
#
#   tools/mkbootdir.sh <image.img> <target-dir> [--kernel-name NAME] [--no-firmware]
#
# Writes into <target-dir>: bootcode.bin, start.elf and fixup.dat from
# tools/fetch_firmware.sh, the image as NAME (default: the image's file
# name), and config.txt with the settings from docs/spec/BOARD_SUPPORT.md.
# --no-firmware skips the firmware files. Copy the result to the card's
# boot partition yourself. This script never formats or writes a device.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)

die() {
    echo "mkbootdir: $*" >&2
    exit 1
}

usage() {
    echo "usage: $0 <image.img> <target-dir> [--kernel-name NAME] [--no-firmware]" >&2
    exit 2
}

image=""
target=""
name=""
firmware=1
while [ $# -gt 0 ]; do
    case "$1" in
    --kernel-name)
        [ $# -ge 2 ] || usage
        name=$2
        shift
        ;;
    --no-firmware) firmware=0 ;;
    -h|--help) usage ;;
    -*) echo "mkbootdir: unknown option $1" >&2; usage ;;
    *)
        if [ -z "$image" ]; then
            image=$1
        elif [ -z "$target" ]; then
            target=$1
        else
            usage
        fi
        ;;
    esac
    shift
done
[ -n "$image" ] && [ -n "$target" ] || usage

[ -f "$image" ] || die "$image is not a regular file"
[ -s "$image" ] || die "$image is empty"
name=${name:-$(basename -- "$image")}

# A plain FAT file name in the partition root.
case "$name" in
*/*|.|..|'') die "bad kernel name '$name'" ;;
esac
[[ "$name" =~ ^[A-Za-z0-9._-]+$ ]] || die "bad kernel name '$name': use letters, digits, '.', '_' or '-'"
case "$(printf '%s' "$name" | tr '[:upper:]' '[:lower:]')" in
bootcode.bin|start.elf|fixup.dat|config.txt) die "kernel name '$name' clashes with a boot file" ;;
esac

case "$target" in
/dev|/dev/*) die "refusing to write to $target: this script never writes a device" ;;
esac
if [ -e "$target" ] && [ ! -d "$target" ]; then
    die "$target exists and is not a directory"
fi
for f in "$name" config.txt; do
    if [ -L "$target/$f" ] || { [ -e "$target/$f" ] && [ ! -f "$target/$f" ]; }; then
        die "refusing to replace $target/$f: not a regular file"
    fi
done

# Check the image before anything is written. The firmware loads the
# kernel at 0x8000, so an image linked anywhere else cannot boot.
python=${PYTHON:-python3}
command -v "$python" > /dev/null || die "cannot verify $image: '$python' not found (set PYTHON)"
"$python" "$here/rimg.py" verify "$image" > /dev/null || die "$image is not a valid rimg image"
load_addr=$("$python" "$here/rimg.py" info "$image" | sed -n 's/^load_addr=//p')
[ "$load_addr" = 0x8000 ] || die "$image is linked at ${load_addr:-an unknown address}, the firmware loads the kernel at 0x8000"

mkdir -p -- "$target"
if [ "$firmware" -eq 1 ]; then
    "$here/fetch_firmware.sh" "$target"
fi
cp -- "$image" "$target/$name"

# Settings from docs/spec/BOARD_SUPPORT.md, section config.txt.
cat > "$target/config.txt" <<EOF
kernel=$name
arm_64bit=0
enable_uart=1
disable_splash=1
boot_delay=0
gpu_mem=64
hdmi_force_hotplug=1
disable_overscan=1
framebuffer_width=640
framebuffer_height=480
framebuffer_depth=32
EOF
echo "mkbootdir: $target is ready. Copy its files to the root of the FAT boot partition."
