#!/usr/bin/env bash
# Boot spike 2 in QEMU with a test SD image and check the @RC read line.
# Usage: run-qemu.sh <raspi0|raspi1ap|raspi2b|raspi3b> [--sdhc]
#   --sdhc  use a 4 GiB sparse image (SDHC) instead of 64 MiB (SDSC)
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
machine="${1:-}"
size_mib=64
card=sdsc
if [[ "${2:-}" == --sdhc ]]; then
    size_mib=4096
    card=sdhc
fi
wait_s="${WAIT_S:-5}"

case "$machine" in
    raspi0|raspi1ap|raspi2b|raspi3b) ;;
    *) echo "usage: $0 <raspi0|raspi1ap|raspi2b|raspi3b> [--sdhc]" >&2; exit 2 ;;
esac

make -C "$here" >/dev/null

elf="$here/build/s2.elf"
img="$here/build/s2.img"
sd="$here/build/sd-$card.img"
log="$here/build/qemu-$machine-$card.log"
rm -f "$log" "$sd"

# Sector 0: byte pattern, one MBR entry (type 0x0c at LBA 2048), 0x55 0xaa.
# Prints the CRC-32 of the sector.
crc="$(python3 -I - "$sd" "$size_mib" <<'PY'
import struct, sys, zlib
path, size_mib = sys.argv[1], int(sys.argv[2])
s = bytearray((i * 7 + 3) & 0xff for i in range(512))
s[446:510] = bytes(64)
s[446:462] = struct.pack("<B3sB3sII", 0, b"\0\0\0", 0x0c, b"\0\0\0", 2048, 131072)
s[510], s[511] = 0x55, 0xaa
with open(path, "wb") as f:
    f.write(s)
    f.truncate(size_mib * 1024 * 1024)
print("0x%08x" % zlib.crc32(bytes(s)))
PY
)"

common=(-M "$machine" -display none -monitor none -serial "file:$log"
        -drive "if=sd,format=raw,file=$sd")

if [[ "$machine" == raspi3b ]]; then
    # raspi3b starts in AArch64. This stub at 0x80000 drops CPU0 to
    # AArch32 HYP at 0x8000: SCR_EL3=0x131, SPSR_EL3=0x1da, ELR_EL3=0x8000, eret.
    stub=(d2802620 d51e1100 d2803b40 d51e4000 d2900000 d51e4020 d69f03e0)
    args=("${common[@]}")
    addr=$((0x80000))
    for word in "${stub[@]}"; do
        args+=(-device "loader,addr=$(printf '0x%x' "$addr"),data=0x$word,data-len=4")
        addr=$((addr + 4))
    done
    args+=(-device "loader,file=$img,addr=0x8000,force-raw=on")
    args+=(-device "loader,addr=0x80000,cpu-num=0")
    qemu=qemu-system-aarch64
else
    args=("${common[@]}" -kernel "$elf")
    qemu=qemu-system-arm
fi

"$qemu" "${args[@]}" 2>"$log.stderr" &
pid=$!
trap 'kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; rm -f "$sd"' EXIT
sleep "$wait_s"
kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true
trap - EXIT
rm -f "$sd"

grep -a '@RC stage=spike2' "$log" | tr -d '\r' | sed "s/^/$machine: /" || true
line="$(grep -a '@RC stage=spike2 ev=read' "$log" | tr -d '\r' | head -n 1 || true)"
cardline="$(grep -a '@RC stage=spike2 ev=card' "$log" | tr -d '\r' | head -n 1 || true)"

fail=0
[[ "$line" == *" ok=1 "* ]] || fail=1
[[ "$line" == *" sig=0xaa55 "* ]] || fail=1
[[ "$line" == *" crc32=$crc "* ]] || fail=1
[[ "$line" == *" p0_type=0x0c p0_lba=2048"* ]] || fail=1
[[ "$cardline" == *" type=$card "* ]] || fail=1
if grep -aq 'ev=exception\|ev=error' "$log"; then
    fail=1
fi

if [[ "$fail" -ne 0 ]]; then
    echo "FAIL: $machine $card, expected crc32=$crc (log: $log)" >&2
    exit 1
fi
echo "PASS: $machine $card"
