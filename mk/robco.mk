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
# Optional: EXTRA_CFLAGS, CI=1 (adds -Werror), CROSS, PYTHON,
# SLOT_END (end of the image's slot, overrides ROBCO_SLOT_END).

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
PYTHON  ?= python3

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

# Every .c and .S under src/<dir>/ goes into librobco.a. Sorted for a stable
# archive order. --gc-sections drops what the image does not use.
ROBCO_SRCS := $(sort $(patsubst $(ROBCO_CORE)/src/%,%,\
              $(wildcard $(ROBCO_CORE)/src/*/*.c $(ROBCO_CORE)/src/*/*.S)))
ROBCO_OBJS := $(addprefix $(BUILD)/robco/,$(addsuffix .o,$(basename $(ROBCO_SRCS))))
# The file system layer: src/fs/ and the unmodified FatFs sources in
# third_party/fatfs/, configured by src/fs/ffconf.h. Only these files get
# the FatFs include paths and the <string.h> in src/fs/libc/.
# ROBCO_FATFS_CFLAGS relaxes warnings for FatFs only.
ROBCO_FS_CFLAGS := -I$(ROBCO_CORE)/src/fs -I$(ROBCO_CORE)/third_party/fatfs -I$(ROBCO_CORE)/src/fs/libc
ROBCO_FATFS_CFLAGS :=
ROBCO_OBJS += $(BUILD)/robco/fatfs/ff.o $(BUILD)/robco/fatfs/ffunicode.o
# SLOT_END reaches the linker script as ROBCO_SLOT_END. robco_config.h
# guards its own value with #ifndef, so SLOT_END wins.
ROBCO_SLOT_DEF := $(if $(strip $(SLOT_END)),-DROBCO_SLOT_END=$(SLOT_END))
PROJ_OBJS  := $(addprefix $(BUILD)/obj/,$(addsuffix .o,$(basename $(SRCS))))

.PHONY: all clean
all: $(BUILD)/$(IMAGE).img

$(BUILD)/$(IMAGE).img: $(BUILD)/$(IMAGE).elf $(ROBCO_CORE)/tools/rimg.py
	$(OBJCOPY) -O binary $< $@.tmp
	$(PYTHON) $(ROBCO_CORE)/tools/rimg.py seal $@.tmp
	mv $@.tmp $@

$(BUILD)/$(IMAGE).elf: $(PROJ_OBJS) $(BUILD)/librobco.a $(BUILD)/image.ld
	$(CC) $(ROBCO_LDFLAGS) -o $@ $(PROJ_OBJS) $(BUILD)/librobco.a -lgcc

$(BUILD)/librobco.a: $(ROBCO_OBJS)
	rm -f $@
	$(AR) rcs $@ $^

$(BUILD)/image.ld: $(ROBCO_CORE)/ld/image.ld.S $(CONFIG_H)
	@mkdir -p $(dir $@)
	$(CC) -E -P -undef -x c -DLINK_ADDR=$(LINK_ADDR) -include $(CONFIG_H) $(ROBCO_SLOT_DEF) -o $@ $<

# Listed before the generic rule so GNU make 3.81 picks it too.
$(BUILD)/robco/fs/%.o: $(ROBCO_CORE)/src/fs/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ROBCO_CFLAGS) $(ROBCO_FS_CFLAGS) -c -o $@ $<

$(BUILD)/robco/fatfs/%.o: $(ROBCO_CORE)/third_party/fatfs/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ROBCO_CFLAGS) $(ROBCO_FS_CFLAGS) $(ROBCO_FATFS_CFLAGS) -c -o $@ $<

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
