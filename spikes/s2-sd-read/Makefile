# Spike 2: read SD sector 0 through the EMMC controller on Raspberry Pi Zero to 3.

CROSS   ?= arm-none-eabi-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy

COMMON  := ../common
BUILD   := build

ARCHFLAGS := -mcpu=arm1176jzf-s -marm -mfloat-abi=soft -mgeneral-regs-only -mno-unaligned-access
CFLAGS    := $(ARCHFLAGS) -DSPIKE_STAGE=spike2 -std=gnu11 -ffreestanding -nostdlib -ffunction-sections -fdata-sections \
             -fno-tree-loop-distribute-patterns -O2 -Wall -Wextra -Werror -I$(COMMON)
ASFLAGS   := $(ARCHFLAGS) -I$(COMMON)
LDFLAGS   := $(ARCHFLAGS) -nostdlib -T $(COMMON)/link.ld -Wl,--gc-sections -Wl,--no-warn-rwx-segments -Wl,-Map,$(BUILD)/s2.map

SRCS_C  := main.c $(COMMON)/board.c $(COMMON)/mbox.c $(COMMON)/uart_pl011.c \
           $(COMMON)/k_printf.c $(COMMON)/k_string.c $(COMMON)/exc.c \
           $(COMMON)/gpio.c $(COMMON)/timer.c $(COMMON)/crc32.c $(COMMON)/emmc.c
SRCS_S  := $(COMMON)/start.S
OBJS    := $(addprefix $(BUILD)/,$(notdir $(SRCS_S:.S=.o) $(SRCS_C:.c=.o)))

vpath %.c . $(COMMON)
vpath %.S $(COMMON)

all: $(BUILD)/s2.img

$(BUILD)/s2.img: $(BUILD)/s2.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/s2.elf: $(OBJS) $(COMMON)/link.ld
	$(CC) $(LDFLAGS) -o $@ $(OBJS) -lgcc

$(BUILD)/%.o: %.c $(wildcard $(COMMON)/*.h) Makefile | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/%.o: %.S | $(BUILD)
	$(CC) $(ASFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
