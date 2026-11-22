# Spike 3: framebuffer and MMU fill rate for Raspberry Pi Zero to 3.

CROSS   ?= arm-none-eabi-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy

COMMON  := ../common
BUILD   := build

ARCHFLAGS := -mcpu=arm1176jzf-s -marm -mfloat-abi=soft -mgeneral-regs-only -mno-unaligned-access
CFLAGS    := $(ARCHFLAGS) -std=gnu11 -ffreestanding -nostdlib -ffunction-sections -fdata-sections \
             -fno-tree-loop-distribute-patterns -O2 -Wall -Wextra -Werror -I$(COMMON)
ASFLAGS   := $(ARCHFLAGS) -I$(COMMON)
LDFLAGS   := $(ARCHFLAGS) -nostdlib -T $(COMMON)/link.ld -Wl,--gc-sections -Wl,--no-warn-rwx-segments -Wl,-Map,$(BUILD)/s3.map

SRCS_C  := main.c $(COMMON)/board.c $(COMMON)/mbox.c $(COMMON)/uart_pl011.c \
           $(COMMON)/k_printf.c $(COMMON)/k_string.c $(COMMON)/exc.c \
           $(COMMON)/fb.c $(COMMON)/mmu.c $(COMMON)/systimer.c
SRCS_S  := $(COMMON)/start.S $(COMMON)/mmu_ops.S
OBJS    := $(addprefix $(BUILD)/,$(notdir $(SRCS_S:.S=.o) $(SRCS_C:.c=.o)))

vpath %.c . $(COMMON)
vpath %.S $(COMMON)

all: $(BUILD)/s3.img

$(BUILD)/s3.img: $(BUILD)/s3.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/s3.elf: $(OBJS) $(COMMON)/link.ld
	$(CC) $(LDFLAGS) -o $@ $(OBJS) -lgcc

$(BUILD)/%.o: %.c $(wildcard $(COMMON)/*.h) | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/%.o: %.S | $(BUILD)
	$(CC) $(ASFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
