"""Guest state snapshot for hang diagnostics. Stdlib only.

collect() builds a text block from HMP commands: the interrupt
controllers and the system timer read on the stopped VM, then CPU
registers sampled several times while it runs. boot.Vm
appends it to the QemuError of a wait that times out; Vm.state_dump()
returns it on demand. The parsers are separate so tool tests can run
them on captured QEMU output.
"""

import os
import re
import shutil
import subprocess
import time

SAMPLES = 5
INTERVAL = 0.1

# Peripheral base per board. docs/spec/BOARD_SUPPORT.md, detection table.
PERIPH_BASE = {
    "raspi0": 0x20000000,
    "raspi1ap": 0x20000000,
    "raspi2b": 0x3F000000,
    "raspi3b": 0x3F000000,
}
# BCM2836/7 core-local controller (BOARD_SUPPORT.md, src/drv/irq_bcm.c).
LOCAL_BASE = {"raspi2b": 0x40000000, "raspi3b": 0x40000000}

# bcm2835-ic at base + 0xB200. Offsets from src/drv/irq_bcm.c.
IC_OFFSET = 0xB200
IC_REGS = ["basic_pending", "pending1", "pending2", "fiq_ctrl",
           "enable1", "enable2", "enable_basic"]
# Local controller offsets. 0x0C is LOCAL_GPU_ROUTING in src/drv/irq_bcm.c.
# The others come from the BCM2836 "QA7" ARM control document, which
# QEMU's bcm2836_control model follows: control, core 0 timer interrupt
# control, core 0 IRQ source.
LOCAL_REGS = [("control", 0x00), ("gpu_routing", 0x0C),
              ("core0_timer_ctrl", 0x40), ("core0_irq_source", 0x60)]
# System timer at base + 0x3000: CS, CLO, CHI, C0-C3. src/drv/timer_sys.c.
ST_OFFSET = 0x3000

AARCH32_MODES = {0x10: "usr", 0x11: "fiq", 0x12: "irq", 0x13: "svc", 0x16: "mon",
                 0x17: "abt", 0x1A: "hyp", 0x1B: "und", 0x1F: "sys"}
AARCH64_MODES = {0x0: "EL0t", 0x4: "EL1t", 0x5: "EL1h", 0x8: "EL2t", 0x9: "EL2h",
                 0xC: "EL3t", 0xD: "EL3h"}

_REG_RE = re.compile(r"\b([A-Za-z][A-Za-z0-9]*)=([0-9a-fA-F]+)\b")


def parse_registers(text):
    """Parse `info registers` of CPU0 into a dict.

    Handles the AArch32 form (R00..R15, PSR) and the AArch64 form
    (X00..X30, SP, PC, PSTATE). Returns keys pc, lr, sp, cpsr, aarch64
    and r0..r12 (x0..x12 in the AArch64 form).
    """
    raw = {}
    for k, v in _REG_RE.findall(text):
        raw.setdefault(k.upper(), int(v, 16))
    if "R15" in raw:
        out = {"pc": raw["R15"], "lr": raw["R14"], "sp": raw["R13"],
               "cpsr": raw["PSR"], "aarch64": False}
        out.update({f"r{i}": raw[f"R{i:02d}"] for i in range(13)})
    elif "PC" in raw and "PSTATE" in raw:
        out = {"pc": raw["PC"], "lr": raw["X30"], "sp": raw["SP"],
               "cpsr": raw["PSTATE"], "aarch64": True}
        out.update({f"r{i}": raw[f"X{i:02d}"] for i in range(13)})
    else:
        raise ValueError("no PC in `info registers` output: " + text[:200].strip())
    return out


def decode_cpsr(value):
    """Mode name and the I, F, T bits of a CPSR (or AArch64 PSTATE)."""
    aarch32 = bool(value & 0x10)
    if aarch32:
        mode = AARCH32_MODES.get(value & 0x1F, f"mode{value & 0x1F:#x}")
    else:
        mode = AARCH64_MODES.get(value & 0xF, f"el{value & 0xF:#x}")
    return {"mode": mode, "I": (value >> 7) & 1, "F": (value >> 6) & 1,
            "T": (value >> 5) & 1 if aarch32 else 0, "aarch32": aarch32}


def format_cpsr(value):
    d = decode_cpsr(value)
    return f"{value:#010x} {d['mode']} I={d['I']} F={d['F']} T={d['T']}"


def parse_xp(text):
    """Words from `xp /Nwx ADDR` output, in address order."""
    words = []
    for line in text.replace("\r", "").splitlines():
        addr, sep, rest = line.partition(":")
        if not sep:
            continue
        words += [int(w, 16) for w in rest.split()]
    return words


def compare_distance(compare, counter):
    """Signed distance from the 32-bit counter to a compare value, in ticks (us).

    Positive: the compare is ahead (fires in that many us). Negative:
    it is behind and only matches after the counter wraps. Wrap-safe.
    """
    d = (compare - counter) & 0xFFFFFFFF
    return d - (1 << 32) if d >= (1 << 31) else d


def format_timer(words):
    """Lines for CS, CLO, CHI, C0-C3, with each compare's distance."""
    cs, clo, chi = words[0], words[1], words[2]
    lines = [f"CS={cs:#010x} (match M3..M0={cs & 0xF:04b}) CLO={clo:#010x} CHI={chi:#010x}"]
    for i, c in enumerate(words[3:7]):
        d = compare_distance(c, clo)
        where = "ahead" if d > 0 else ("behind" if d < 0 else "equal")
        lines.append(f"C{i}={c:#010x} match={(cs >> i) & 1} distance={d:+d} us ({where})")
    return lines


def summary(samples):
    """One line: was the guest still executing, were IRQs masked."""
    first = samples[0]
    pc_moved = any(s["pc"] != first["pc"] for s in samples)
    keys = ["pc", "lr", "sp", "cpsr"] + [f"r{i}" for i in range(13)]
    regs_moved = any(s[k] != first[k] for s in samples for k in keys)
    n = len(samples)
    if pc_moved:
        run = "yes (pc changed between samples)"
    elif regs_moved:
        run = "yes (pc fixed, registers changed)"
    else:
        run = f"no (pc and registers fixed over {n} samples)"
    masked = [decode_cpsr(s["cpsr"])["I"] for s in samples]
    if all(masked):
        irq = "IRQs masked (CPSR.I=1 in every sample)"
    elif not any(masked):
        irq = "IRQs unmasked (CPSR.I=0 in every sample)"
    else:
        irq = f"IRQs masked in {sum(masked)} of {n} samples"
    return f"summary: guest executing: {run}; {irq}"


def find_elf(image):
    """The ELF next to `image` (foo.img -> foo.elf), or None."""
    if image is None:
        return None
    root, _ = os.path.splitext(os.fspath(image))
    elf = root + ".elf"
    return elf if os.path.exists(elf) else None


def resolve(addrs, elf, timeout=5.0):
    """Map addresses to symbol names. Returns (dict, note).

    Never raises. The note says where names came from, or why they are
    missing.
    """
    if not elf:
        return {}, "symbols: none, no ELF for this image (pass elf= to boot())"
    if not os.path.exists(elf):
        return {}, f"symbols: none, ELF not found: {elf}"
    uniq = sorted(set(addrs))
    tool = shutil.which("arm-none-eabi-addr2line")
    try:
        if tool:
            out = subprocess.run([tool, "-f", "-e", elf] + [f"{a:#x}" for a in uniq],
                                 capture_output=True, text=True, timeout=timeout,
                                 check=True).stdout.splitlines()
            names = {a: out[2 * i] for i, a in enumerate(uniq) if 2 * i < len(out)}
            return names, f"symbols: {os.path.basename(elf)} via arm-none-eabi-addr2line"
        tool = shutil.which("arm-none-eabi-nm")
        if tool:
            out = subprocess.run([tool, "-n", elf], capture_output=True, text=True,
                                 timeout=timeout, check=True).stdout
            return (nm_lookup(out, uniq),
                    f"symbols: {os.path.basename(elf)} via arm-none-eabi-nm")
        return {}, "symbols: none, arm-none-eabi-addr2line and arm-none-eabi-nm not found"
    except Exception as e:  # noqa: BLE001 - symbols are optional
        reason = (f"timed out after {e.timeout:.1f}s" if isinstance(e, subprocess.TimeoutExpired)
                  else str(e))
        return {}, f"symbols: none, lookup failed: {reason}"


def nm_lookup(nm_text, addrs):
    """Nearest preceding text symbol from `nm -n` output for each address."""
    syms = []
    for line in nm_text.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in "tTwW" and not parts[2].startswith("$"):
            syms.append((int(parts[0], 16), parts[2]))
    names = {}
    for a in addrs:
        best = None
        for value, name in syms:
            if value <= a:
                best = f"{name}+{a - value:#x}" if a != value else name
            else:
                break
        names[a] = best or "??"
    return names


class OverBudget(Exception):
    """The dump's time budget ran out before a step."""


def collect(hmp, machine, elf=None, samples=SAMPLES, interval=INTERVAL, sleep=time.sleep,
            deadline=None, clock=time.monotonic, pause=None, resume=None):
    """Build the snapshot text. `hmp(cmd)` runs one HMP command.

    Order: pause() (QMP stop), the device registers (interrupt
    controllers, system timer), one CPU0 sample of the stopped VM,
    resume() (QMP cont), then `samples` CPU0 samples of the running VM,
    then symbols. resume() runs whenever pause() was tried, also when a
    step failed or the budget ran out. Without pause the devices are
    read on the running VM and the block says so.

    `deadline` is a `clock()` time. Every step checks it first, and the
    symbol lookup gets only what is left. A failed or late step costs
    only its own part: the block keeps everything read so far and adds
    one `not collected:` line per missing part. Raises only when nothing
    could be read.
    """
    missing, errors = [], []

    def left():
        return None if deadline is None else deadline - clock()

    def step(name, fn):
        rem = left()
        if rem is not None and rem <= 0:
            missing.append(f"not collected: {name}: over the time budget")
            return None
        try:
            return fn()
        except Exception as e:  # noqa: BLE001 - keep what was read
            errors.append(e)
            missing.append(f"not collected: {name}: {type(e).__name__}: {e}")
            return None

    def sample(name):
        text = step(name, lambda: hmp("info registers").replace("\r", ""))
        if text is None:
            return None, None
        return step(name, lambda: parse_registers(text)), text

    stopped, tried_pause = False, False
    hw, stop_regs, first_text = [], None, None
    try:
        if pause is not None:
            tried_pause = True
            stopped = step("stop the VM", lambda: pause() or True) is not None
        base = PERIPH_BASE.get(machine)
        if base is None:
            hw.append(f"peripherals: unknown board {machine!r}, skipped")
        else:
            ic = step("interrupt controller",
                      lambda: parse_xp(hmp(f"xp /{len(IC_REGS)}wx {base + IC_OFFSET:#x}")))
            if ic is not None:
                hw.append(f"interrupt controller @ {base + IC_OFFSET:#010x}:")
                hw.append("  " + " ".join(f"{n}={v:#010x}" for n, v in zip(IC_REGS, ic)))
            local = LOCAL_BASE.get(machine)
            if local is not None:
                def read_local():
                    return [f"{n}={parse_xp(hmp(f'xp /1wx {local + off:#x}'))[0]:#010x}"
                            for n, off in LOCAL_REGS]
                vals = step("local controller", read_local)
                if vals is not None:
                    hw.append(f"local controller @ {local:#010x}:")
                    hw.append("  " + " ".join(vals))
            st = step("system timer",
                      lambda: format_timer(parse_xp(hmp(f"xp /7wx {base + ST_OFFSET:#x}"))))
            if st is not None:
                hw.append(f"system timer @ {base + ST_OFFSET:#010x}:")
                hw += ["  " + line for line in st]
        if stopped:
            stop_regs, first_text = sample("cpu0 sample of the stopped VM")
    finally:
        resumed = None
        if tried_pause:
            try:
                resume()
                resumed = True
            except Exception as e:  # noqa: BLE001 - say so, keep the rest
                errors.append(e)
                resumed = False
                missing.append(f"not collected: resume the VM (cont): {type(e).__name__}: {e};"
                               " the VM may still be stopped")

    regs = []
    for i in range(samples):
        if i:
            sleep(interval)
        r, text = sample(f"cpu0 sample #{i + 1} of {samples}")
        if r is None:
            break
        first_text = first_text or text
        regs.append(r)

    if not regs and not hw and stop_regs is None:
        raise errors[0] if errors else OverBudget("over the time budget before the first read")

    # Symbols last, with only the remaining budget.
    all_regs = regs + ([stop_regs] if stop_regs else [])
    names, sym_note = {}, None
    if all_regs:
        rem = left()
        if rem is not None and rem <= 0:
            sym_note = "symbols: none, over the time budget before the lookup"
        else:
            names, sym_note = resolve([a for r in all_regs for a in (r["pc"], r["lr"])], elf,
                                      timeout=5.0 if rem is None else min(5.0, rem))

    def sym(a):
        return f" <{names[a]}>" if names.get(a) else ""

    def fmt(r):
        return (f"pc={r['pc']:#010x}{sym(r['pc'])} lr={r['lr']:#010x}{sym(r['lr'])}"
                f" sp={r['sp']:#010x} cpsr={format_cpsr(r['cpsr'])}")

    out = [summary(regs) if regs else "summary: guest executing: unknown (no running samples)"]
    if stopped:
        out.append("device registers: read with the VM stopped (QMP stop, then cont"
                   + (")" if resumed else ", which failed)"))
    elif tried_pause:
        out.append("device registers: read with the VM running (QMP stop failed)")
    else:
        out.append("device registers: read with the VM running")
    if stop_regs:
        out.append(f"cpu0 at stop: {fmt(stop_regs)}")
    if regs:
        out.append(f"cpu0 samples while running ({len(regs)}, {int(interval * 1000)} ms apart):")
        out += [f"  #{i} {fmt(r)}" for i, r in enumerate(regs, 1)]
    if all_regs:
        out.append(sym_note)
    out += hw
    out += missing
    if first_text is not None:
        out.append("info registers (first sample):")
        out += ["  " + line for line in core_registers(first_text)]
    return out


_FP_LINE = re.compile(r"^(s\d\d=|Q\d\d=|FPSCR)")


def core_registers(text):
    """`info registers` lines without the floating-point and SIMD ones."""
    return [line for line in text.strip("\n").splitlines()
            if line.strip() and not _FP_LINE.match(line.strip())]


def block(lines, machine, title="guest state at timeout"):
    head = f"--- {title} ({machine}) ---"
    return "\n".join([head] + lines + ["--- end of guest state ---"])
