# Firmware stub, loaded raw at 0x0 by tests/harness/boot.py.

CROSS   ?= arm-none-eabi-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
BUILD   := build

all: $(BUILD)/fwstub.bin

$(BUILD)/fwstub.bin: $(BUILD)/fwstub.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/fwstub.elf: fwstub.S Makefile | $(BUILD)
	$(CC) -mcpu=arm1176jzf-s -marm -nostdlib -Wl,-Ttext=0x0 -Wl,--build-id=none \
	    -Wl,--no-warn-rwx-segments -o $@ $<

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
