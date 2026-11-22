#!/usr/bin/env bash
# Boot spike 1 in QEMU and check the @RC hello line.
# Usage: run-qemu.sh <raspi0|raspi1ap|raspi2b|raspi3b>
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
machine="${1:-}"
wait_s="${WAIT_S:-4}"

case "$machine" in
    raspi0|raspi1ap) soc=bcm2835; base=0x20000000; mode="" ;;
    raspi2b)         soc=bcm2836; base=0x3f000000; mode="" ;;
    raspi3b)         soc=bcm2837; base=0x3f000000; mode=hyp ;;
    *) echo "usage: $0 <raspi0|raspi1ap|raspi2b|raspi3b>" >&2; exit 2 ;;
esac

make -C "$here" >/dev/null

elf="$here/build/s1.elf"
img="$here/build/s1.img"
log="$here/build/qemu-$machine.log"
rm -f "$log"

common=(-M "$machine" -display none -monitor none -serial "file:$log")

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
trap 'kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true' EXIT
sleep "$wait_s"
kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true
trap - EXIT

line="$(grep -a '@RC stage=spike1 ev=hello' "$log" | tr -d '\r' | head -n 1 || true)"
echo "$machine: ${line:-<no @RC line>}"

fail=0
[[ -n "$line" ]] || fail=1
[[ "$line" == *" soc=$soc "* ]] || fail=1
[[ "$line" == *" base=$base "* ]] || fail=1
if [[ -n "$mode" ]]; then
    [[ "$line" == *" entry_mode=$mode "* ]] || fail=1
fi
if grep -aq 'ev=exception' "$log"; then
    fail=1
fi

if [[ "$fail" -ne 0 ]]; then
    echo "FAIL: $machine (log: $log)" >&2
    exit 1
fi
echo "PASS: $machine"
