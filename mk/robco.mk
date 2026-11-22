# RobCo-Core build fragment. Include it at the end of a project Makefile:
#
#   ROBCO_CORE := path/to/RobCo-Core
#   IMAGE      := name             (output base name)
#   LINK_ADDR  := 0x8000
#   SRCS       := main.c ...       (project C/asm files, relative paths)
#   CONFIG_H   := robco_config.h
#   BUILD      ?= build            (optional)
#   include $(ROBCO_CORE)/mk/robco.mk
#
# Optional: EXTRA_CFLAGS, CI=1 (adds -Werror), CROSS.

ifeq ($(strip $(ROBCO_CORE)),)
$(error ROBCO_CORE is not set)
endif
ifeq ($(strip $(IMAGE)),)
$(error IMAGE is not set)
endif
ifeq ($(strip $(LINK_ADDR)),)
$(error LINK_ADDR is not set)
endif
ifeq ($(strip $(CONFIG_H)),)
$(error CONFIG_H is not set)
endif

BUILD   ?= build
CROSS   ?= arm-none-eabi-
CC      := $(CROSS)gcc
AR      := $(CROSS)ar
LD      := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy

ROBCO_ARCHFLAGS := -mcpu=arm1176jzf-s -marm -mfloat-abi=soft -mgeneral-regs-only -mno-unaligned-access
ROBCO_CFLAGS := $(ROBCO_ARCHFLAGS) -std=gnu11 -ffreestanding -nostdlib \
                -ffunction-sections -fdata-sections -fno-tree-loop-distribute-patterns \
                -O2 -g -Wall -Wextra -MMD -MP \
                -I$(ROBCO_CORE)/include -I$(dir $(CONFIG_H)) -include $(CONFIG_H) \
                $(EXTRA_CFLAGS)
ifeq ($(CI),1)
ROBCO_CFLAGS += -Werror
endif

# --no-warn-rwx-segments needs binutils 2.39 or later.
ROBCO_LD_RWX := $(shell $(LD) --help 2>/dev/null | grep -q no-warn-rwx-segments && echo -Wl,--no-warn-rwx-segments)
ROBCO_LDFLAGS := $(ROBCO_ARCHFLAGS) -nostdlib -T $(BUILD)/image.ld \
                 -Wl,--gc-sections -Wl,-u,_start -Wl,-Map,$(BUILD)/$(IMAGE).map $(ROBCO_LD_RWX)

ROBCO_SRCS := arch/start.S arch/vectors.S arch/exceptions.c board/board.c \
              drv/uart_pl011.c drv/mbox.c lib/k_string.c lib/k_printf.c
ROBCO_OBJS := $(addprefix $(BUILD)/robco/,$(addsuffix .o,$(basename $(ROBCO_SRCS))))
PROJ_OBJS  := $(addprefix $(BUILD)/obj/,$(addsuffix .o,$(basename $(SRCS))))

.PHONY: all clean
all: $(BUILD)/$(IMAGE).img

$(BUILD)/$(IMAGE).img: $(BUILD)/$(IMAGE).elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/$(IMAGE).elf: $(PROJ_OBJS) $(BUILD)/librobco.a $(BUILD)/image.ld
	$(CC) $(ROBCO_LDFLAGS) -o $@ $(PROJ_OBJS) $(BUILD)/librobco.a -lgcc

$(BUILD)/librobco.a: $(ROBCO_OBJS)
	rm -f $@
	$(AR) rcs $@ $^

$(BUILD)/image.ld: $(ROBCO_CORE)/ld/image.ld.S $(CONFIG_H)
	@mkdir -p $(dir $@)
	$(CC) -E -P -undef -x c -DLINK_ADDR=$(LINK_ADDR) -include $(CONFIG_H) -o $@ $<

$(BUILD)/robco/%.o: $(ROBCO_CORE)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ROBCO_CFLAGS) -c -o $@ $<

$(BUILD)/robco/%.o: $(ROBCO_CORE)/src/%.S
	@mkdir -p $(dir $@)
	$(CC) $(ROBCO_CFLAGS) -c -o $@ $<

$(BUILD)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(ROBCO_CFLAGS) -c -o $@ $<

$(BUILD)/obj/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ROBCO_CFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILD)

-include $(ROBCO_OBJS:.o=.d) $(PROJ_OBJS:.o=.d)
