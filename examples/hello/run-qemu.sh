#!/usr/bin/env bash
# Boot the hello example in QEMU and check its @RC lines.
# Usage: run-qemu.sh <raspi0|raspi1ap|raspi2b|raspi3b> [--fault]
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
machine="${1:-}"
fault="${2:-}"
wait_s="${WAIT_S:-4}"

case "$machine" in
    raspi0|raspi1ap) soc=bcm2835; base=0x20000000; pwm=abort; mode="" ;;
    raspi2b)         soc=bcm2836; base=0x3f000000; pwm=ok;    mode="" ;;
    raspi3b)         soc=bcm2837; base=0x3f000000; pwm=ok;    mode=hyp ;;
    *) echo "usage: $0 <raspi0|raspi1ap|raspi2b|raspi3b> [--fault]" >&2; exit 2 ;;
esac

case "$fault" in
    "")      build=build;       extra="" ;;
    --fault) build=build/fault; extra="-DHELLO_FAULT=1" ;;
    *) echo "unknown option: $fault" >&2; exit 2 ;;
esac

make -C "$here" BUILD="$build" EXTRA_CFLAGS="$extra" >/dev/null

elf="$here/$build/hello.elf"
img="$here/$build/hello.img"
log="$here/$build/qemu-$machine.log"
rm -f "$log"

# The rimg header sits at offset 0x20 and starts with the magic.
magic="$(dd if="$img" bs=1 skip=32 count=7 2>/dev/null)"
if [[ "$magic" != "RCHELLO" ]]; then
    echo "FAIL: $machine: bad rimg magic '$magic' in $img" >&2
    exit 1
fi

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

grab() { grep -a "@RC stage=hello ev=$1" "$log" | tr -d '\r' | head -n 1 || true; }
boot="$(grab boot)"
probe="$(grab probe)"
ready="$(grab ready)"
exc="$(grab exception)"
echo "$machine: ${boot:-<no ev=boot>}"
echo "$machine: ${probe:-<no ev=probe>}"
[[ -z "$exc" ]] || echo "$machine: $exc"

fail=0
[[ "$boot" == *" soc=$soc "* ]] || fail=1
[[ "$boot" == *" base=$base "* ]] || fail=1
if [[ -n "$mode" ]]; then
    [[ "$boot" == *" entry_mode=$mode"* ]] || fail=1
fi
[[ "$probe" == *" pwm=$pwm gpio=ok"* ]] || fail=1
[[ -n "$ready" ]] || fail=1
if [[ -z "$fault" ]]; then
    [[ -z "$exc" ]] || fail=1
else
    [[ "$exc" == *" type=undef "* ]] || fail=1
fi

if [[ "$fail" -ne 0 ]]; then
    echo "FAIL: $machine $fault (log: $log)" >&2
    exit 1
fi
echo "PASS: $machine $fault"
