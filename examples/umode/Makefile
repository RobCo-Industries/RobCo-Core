# Runs test programs in USR mode: system calls, faults, kills, with the
# MMU off and then on. The programs in user/ are a separate raw binary
# linked at 0x00400000 and embedded in the kernel image.

ROBCO_CORE ?= ../..
IMAGE      := umode
LINK_ADDR  := 0x8000
SRCS       := main.c user_blob.S
CONFIG_H   := robco_config.h

include $(ROBCO_CORE)/mk/robco.mk

USER_BIN := $(BUILD)/user/prog.bin

$(BUILD)/user/prog.elf: user/prog.S user/prog.ld
	@mkdir -p $(dir $@)
	$(CC) -mcpu=arm1176jzf-s -marm -nostdlib -Wl,-T,user/prog.ld $(ROBCO_LD_RWX) -o $@ $<

$(USER_BIN): $(BUILD)/user/prog.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/obj/user_blob.o: $(USER_BIN)
$(BUILD)/obj/user_blob.o: ROBCO_CFLAGS += -Wa,-I$(BUILD)/user
