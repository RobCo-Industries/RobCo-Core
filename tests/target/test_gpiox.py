"""Boot examples/gpiox on every board: PCA9554 expanders at 0x20 and 0x21.

QEMU's pca9554 model is canary V17. The test observes output pins with
QMP qom-get of pinN, which gives the configuration bit OR the output bit,
and drives input pins through qtest set_irq_in while the guest runs. A driven level of 1 releases a
line.
qom-set is never used: it rewrites the chip's registers. Nothing answers
at 0x22.
"""

import re

from harness.boot import boot

DONE = re.compile(r"@RC stage=gpiox ev=done\n")
ADDRS = (0x20, 0x21)
DEVICES = []
for i, a in enumerate(ADDRS):
    DEVICES += ["-device", f"pca9554,bus=i2c-bus.1,address={a:#x},id=gx{i}"]
# Inputs get the next pattern. The model ANDs an outside level with the
# chip's own drive (canary V17), so the last input pattern of a phase is
# 0xF: it releases every line before the pins change direction.
PATTERNS = (0xF, 0x0, 0x5, 0xA)
STEPS = len(PATTERNS)

GPIOX_ERR_ARG, GPIOX_ERR_ADDR, GPIOX_ERR_ABSENT = "-30", "-31", "-32"
GPIOX_ERR_BUSY, GPIOX_ERR_DETACHED = "-34", "-35"


def path(chip):
    return f"/machine/peripheral/gx{chip}"


def out_mask(chip, phase):
    return 0xF0 if (chip ^ phase) & 1 else 0x0F


def spread(pat, mask):
    return pat if mask == 0x0F else pat << 4


def events(vm, name, **match):
    want = {k: str(v) for k, v in match.items()}
    return [m for m in vm.markers() if m.get("stage") == "gpiox" and m.get("ev") == name
            and all(m.get(k) == v for k, v in want.items())]


def event(vm, name, **match):
    found = events(vm, name, **match)
    assert len(found) == 1, vm.serial
    return found[0]


def pins(vm, chip):
    return sum((vm.qmp.execute("qom-get", path=path(chip), property=f"pin{p}") == "high") << p
               for p in range(8))


def drive(vm, chip, mask, bits):
    for p in range(8):
        if mask >> p & 1:
            vm.qmp.set_irq_in(path(chip), p, bits >> p & 1)


def check_errors(vm):
    e = event(vm, "errors")
    assert (e["index"], e["addr"], e["absent"]) == (GPIOX_ERR_ARG, GPIOX_ERR_ADDR,
                                                    GPIOX_ERR_ABSENT), vm.serial
    assert (e["detached"], e["name"]) == (GPIOX_ERR_DETACHED, GPIOX_ERR_ARG), vm.serial
    absent = event(vm, "absent")
    assert (absent["rc"], absent["present"]) == (GPIOX_ERR_ABSENT, "0"), vm.serial
    assert not [m for m in vm.markers() if m.get("ev") == "exception"], vm.serial


def test_gpiox_both_directions(board, example_image):
    with boot(example_image("gpiox"), board, extra_args=DEVICES, qtest=True) as vm:
        vm.wait_for(re.compile(r"ev=busy rc=\S+\n"), timeout=60)
        assert event(vm, "i2c")["rc"] == "0", vm.serial
        for chip, a in enumerate(ADDRS):
            att = event(vm, "attach", chip=chip)
            assert (att["addr"], att["rc"], att["present"]) == (f"{a:#04x}", "0", "1"), vm.serial
            assert att["last"] == f"X{chip}.7"
        assert event(vm, "busy")["rc"] == GPIOX_ERR_BUSY

        checked = {(c, p, d): 0 for c in range(2) for p in range(8) for d in ("out", "in")}
        for phase in range(2):
            for k in range(STEPS):
                vm.wait_for(f"ev=wait phase={phase} step={k}\n", timeout=30)
                for chip in range(2):
                    cfg = event(vm, "config", phase=phase, chip=chip)
                    mask = out_mask(chip, phase)
                    assert (cfg["rc"], int(cfg["cfg"], 16)) == ("0", 0xFF & ~mask), vm.serial
                    out = spread(PATTERNS[k], mask)
                    o = event(vm, "out", phase=phase, step=k, chip=chip)
                    assert (o["rc"], o["rrc"]) == ("0", "0"), vm.serial
                    # The chip reads its outputs back in the input register.
                    assert int(o["read"], 16) & mask == out, vm.serial
                    # Outside QEMU: outputs at their level, inputs high.
                    assert pins(vm, chip) == out | (0xFF & ~mask), (phase, k, chip)
                    # Drive the inputs with another pattern.
                    drive(vm, chip, 0xFF & ~mask,
                          spread(PATTERNS[(k + 1) % STEPS], 0xFF & ~mask))
                    for p in range(8):
                        checked[(chip, p, "out" if mask >> p & 1 else "in")] += 1
                vm.send("g")
                vm.wait_for(re.compile(rf"ev=in phase={phase} step={k} chip=1 .*\n"), timeout=30)
                for chip in range(2):
                    mask = out_mask(chip, phase)
                    want = spread(PATTERNS[k], mask) | spread(PATTERNS[(k + 1) % STEPS],
                                                             0xFF & ~mask)
                    i = event(vm, "in", phase=phase, step=k, chip=chip)
                    assert i["rc"] == "0", vm.serial
                    assert int(i["read"], 16) == want, (phase, k, chip, vm.serial)
                    assert int(i["pins"], 16) == want, (phase, k, chip, vm.serial)
        # Every pin ran as an output and as an input, and saw 0 and 1 each way.
        assert all(n == STEPS for n in checked.values()), checked

        vm.wait_for(DONE, timeout=30)
        d = event(vm, "detach")
        assert (d["rc"], d["present"], d["write"]) == ("0", "0", GPIOX_ERR_DETACHED), vm.serial
        check_errors(vm)


def test_gpiox_without_expanders(board, example_image):
    with boot(example_image("gpiox"), board) as vm:
        vm.wait_for(DONE, timeout=60)
        for chip in range(2):
            att = event(vm, "attach", chip=chip)
            assert (att["rc"], att["present"]) == (GPIOX_ERR_ABSENT, "0"), vm.serial
        assert not events(vm, "config"), vm.serial
        check_errors(vm)
