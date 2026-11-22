"""Boot examples/rtc on every board: I2C on BSC1, DS1307 driver, software clock.

QEMU's ds1338 model has the DS1307 register layout. Every board names
the BSC1 bus i2c-bus.1. The model follows the host clock until the guest
sets it, so the first reading is compared loosely with the host UTC time.
The model ignores the clock-halt bit and has no clock stretching, so
RTC_ERR_HALTED and I2C_ERR_CLKT are not tested here.
"""

import datetime
import re

from harness.boot import boot

DONE = re.compile(r"@RC stage=rtc ev=done\n")
RTC_DEVICE = ["-device", "ds1338,bus=i2c-bus.1,address=0x68"]
FIXED = "2077-10-23T09:47:05"

I2C_ERR_ARG, I2C_ERR_NACK = "-1", "-3"
RTC_ERR_ABSENT = "-11"


def event(vm, name):
    found = [m for m in vm.markers() if m.get("stage") == "rtc" and m.get("ev") == name]
    assert len(found) == 1, vm.serial
    return found[0]


def parse_iso(text):
    return datetime.datetime.strptime(text, "%Y-%m-%dT%H:%M:%S")


def check_common(vm):
    assert event(vm, "i2c")["rc"] == "0", vm.serial
    assert int(event(vm, "i2c")["hz"]) <= 100000
    errors = event(vm, "errors")
    assert errors["zero"] == I2C_ERR_ARG
    assert errors["addr"] == I2C_ERR_ARG
    assert errors["nack"] == I2C_ERR_NACK
    assert errors["nack2"] == I2C_ERR_NACK
    assert errors["after"] == "0"
    assert not [m for m in vm.markers() if m.get("ev") == "exception"], vm.serial


def test_rtc_present(board, example_image):
    host_now = datetime.datetime.now(datetime.timezone.utc).replace(tzinfo=None)
    with boot(example_image("rtc"), board, extra_args=RTC_DEVICE) as vm:
        vm.wait_for(DONE, timeout=60)
        check_common(vm)

        scan = event(vm, "scan")
        assert scan["found"] == "0x68", vm.serial
        assert scan["errors"] == "0"

        rtc = event(vm, "rtc")
        assert (rtc["present"], rtc["running"], rtc["rc"]) == ("1", "1", "0"), vm.serial
        # The ds1338 RAM does not repeat the time at 0x13: the DS1307 family.
        assert rtc["chip"] == "0", vm.serial

        # QEMU's RTC follows the host clock: compare loosely.
        t = event(vm, "time")
        assert t["rc"] == "0", vm.serial
        assert abs(parse_iso(t["iso"]) - host_now) < datetime.timedelta(days=1)
        clock0 = event(vm, "clock0")
        assert clock0["state"] == "set", vm.serial
        assert parse_iso(clock0["iso"]) <= parse_iso(t["iso"])

        nv = event(vm, "nvram")
        assert (nv["write"], nv["read"], nv["same"]) == ("0", "0", "1"), vm.serial

        h12 = event(vm, "h12")
        assert (h12["write"], h12["rc"], h12["hour"]) == ("0", "0", "23"), vm.serial

        s = event(vm, "set")
        assert (s["rc"], s["rrc"]) == ("0", "0"), vm.serial
        assert s["iso"] == FIXED
        assert s["readback"] == FIXED

        after = event(vm, "after")
        assert after["rc"] == "0", vm.serial
        assert parse_iso(after["iso"]) - parse_iso(FIXED) >= datetime.timedelta(seconds=2)

        clock = event(vm, "clock")
        assert clock["state"] == "set", vm.serial
        assert parse_iso(clock["iso"]) - parse_iso(FIXED) >= datetime.timedelta(seconds=2)


def test_rtc_absent(board, example_image):
    with boot(example_image("rtc"), board) as vm:
        vm.wait_for(DONE, timeout=60)
        check_common(vm)

        scan = event(vm, "scan")
        assert scan["found"] == "", vm.serial
        assert scan["errors"] == "0"

        rtc = event(vm, "rtc")
        assert (rtc["present"], rtc["running"], rtc["rc"]) == ("0", "0", RTC_ERR_ABSENT)
        assert rtc["chip"] == "-1", vm.serial
        absent = event(vm, "absent")
        assert absent == {"stage": "rtc", "ev": "absent", "get": RTC_ERR_ABSENT,
                          "set": RTC_ERR_ABSENT, "running": RTC_ERR_ABSENT}
        assert event(vm, "clock0")["state"] == "unset"
        assert event(vm, "clock")["state"] == "unset"
