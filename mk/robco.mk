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
# `make sdimage` writes $(BUILD)/sd.img with tools/mksdimage.sh from
# SDIMAGE_DIR, SDIMAGE_FILES and sdimage-stage:: rules (see sdimage
# below). SDIMAGE_DEPS lists extra prerequisites. SDIMAGE_MIB sets the
# size, 64 by default.

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
# The files this fragment links or generates. The rules below use these
# names, so the spelling check removes exactly what the rules make.
ROBCO_LIB := $(BUILD)/librobco.a
ROBCO_LDS := $(BUILD)/image.ld
ROBCO_ELF := $(BUILD)/$(IMAGE).elf
ROBCO_MAP := $(BUILD)/$(IMAGE).map
ROBCO_IMG := $(BUILD)/$(IMAGE).img
ROBCO_OUTPUTS := $(ROBCO_LIB) $(ROBCO_LDS) $(ROBCO_ELF) $(ROBCO_MAP) $(ROBCO_IMG) $(ROBCO_IMG).tmp
# gcc names the object in each .d file as the rule spelled BUILD, so
# `BUILD=build` and an absolute BUILD for the same folder miss each other's
# header dependencies. $(BUILD)/.spelling records the spelling of the last
# run. When it changes, the fragment deletes the .o and .d files under
# $(BUILD)/obj and $(BUILD)/robco and every file in ROBCO_OUTPUTS, so all of
# them build again in the new spelling. It keeps every other file in
# $(BUILD), such as the sdimage outputs sd.img and sdstage/.
# Deleted rather than made older: GNU make 3.81 sees mtimes in whole seconds.
# For the same reason the linked outputs go too: rebuilt objects can share
# one second with the old archive, and 3.81 would keep the old image.
ROBCO_SPELLING := $(BUILD)/.spelling
ifneq ($(strip $(BUILD)),)
ifneq ($(shell cat '$(ROBCO_SPELLING)' 2>/dev/null),$(BUILD))
$(shell mkdir -p '$(BUILD)' && \
  find '$(BUILD)/obj' '$(BUILD)/robco' \( -name '*.o' -o -name '*.d' \) -type f -exec rm -f {} + 2>/dev/null; \
  rm -f $(foreach f,$(ROBCO_OUTPUTS),'$(f)'); \
  printf '%s\n' '$(BUILD)' > '$(ROBCO_SPELLING)')
endif
endif
CROSS  ?= arm-none-eabi-
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
ROBCO_LDFLAGS := $(ROBCO_ARCHFLAGS) -nostdlib -T $(ROBCO_LDS) \
                 -Wl,--gc-sections -Wl,-u,_start -Wl,-Map,$(ROBCO_MAP) $(ROBCO_LD_RWX)

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
all: $(ROBCO_IMG)

$(ROBCO_IMG): $(ROBCO_ELF) $(ROBCO_CORE)/tools/rimg.py
	$(OBJCOPY) -O binary $< $@.tmp
	$(PYTHON) $(ROBCO_CORE)/tools/rimg.py seal $@.tmp
	mv $@.tmp $@

$(ROBCO_ELF): $(PROJ_OBJS) $(ROBCO_LIB) $(ROBCO_LDS)
	$(CC) $(ROBCO_LDFLAGS) -o $@ $(PROJ_OBJS) $(ROBCO_LIB) -lgcc

$(ROBCO_LIB): $(ROBCO_OBJS)
	rm -f $@
	$(AR) rcs $@ $^

$(ROBCO_LDS): $(ROBCO_CORE)/ld/image.ld.S $(CONFIG_H)
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

# `make sdimage` stages the card in $(SDIMAGE_STAGE), $(BUILD)/sdstage,
# then packs it:
#   1. builds SDIMAGE_DEPS and the sources of SDIMAGE_FILES;
#   2. copies the tree in SDIMAGE_DIR, if set;
#   3. copies each SDIMAGE_FILES pair "source:destination", in order,
#      the destination relative to the card root:
#      - a file source becomes exactly the destination path, such as
#        "$(BUILD)/app.img:APP.IMG" or "boot/APP.CFG:APP/APP.CFG". A
#        destination that ends in "/" is a directory: the file goes into
#        it under its own name, such as "boot/APP.CFG:APP/";
#      - a directory source copies its contents into the destination
#        directory, such as "assets:APP/ASSETS". The copy merges with
#        what is there.
#      Missing parent directories are created. A later pair overwrites
#      what an earlier pair or SDIMAGE_DIR put at the same path. Paths
#      cannot contain spaces;
#   4. runs the double-colon target sdimage-stage. A project adds its own
#      steps with "sdimage-stage::" rules that write into $(SDIMAGE_STAGE);
#   5. writes $(BUILD)/sd.img with tools/mksdimage.sh.
SDIMAGE_MIB   ?= 64
SDIMAGE_STAGE := $(BUILD)/sdstage
ROBCO_SD_SRCS := $(foreach p,$(SDIMAGE_FILES),$(firstword $(subst :, ,$(p))))
.PHONY: sdimage sdimage-stage
sdimage: $(SDIMAGE_DEPS) $(ROBCO_SD_SRCS)
	$(if $(strip $(BUILD)),,$(error BUILD is empty))
	$(if $(strip $(SDIMAGE_DIR)$(SDIMAGE_FILES)$(SDIMAGE_DEPS)),,$(error nothing for the card: set SDIMAGE_DIR, SDIMAGE_FILES or SDIMAGE_DEPS))
	rm -rf $(SDIMAGE_STAGE)
	mkdir -p $(SDIMAGE_STAGE)
	$(if $(word 2,$(SDIMAGE_DIR)),$(error SDIMAGE_DIR cannot contain spaces))
	$(if $(strip $(SDIMAGE_DIR)),cp -R $(SDIMAGE_DIR)/. $(SDIMAGE_STAGE)/)
	@set -e; stage='$(SDIMAGE_STAGE)'; for p in $(SDIMAGE_FILES); do \
	  case "$$p" in *:?*) ;; *) echo "SDIMAGE_FILES: $$p is not source:destination" >&2; exit 1 ;; esac; \
	  src=$${p%%:*}; dst=$${p#*:}; \
	  if [ -d "$$src" ]; then \
	    out=$$stage/$${dst%/}; \
	    if [ -e "$$out" ] && [ ! -d "$$out" ]; then \
	      echo "SDIMAGE_FILES: $$p: $$src is a directory, $$dst a file" >&2; exit 1; fi; \
	    echo "cp -R $$src/. $$out/"; mkdir -p "$$out"; cp -R "$$src/." "$$out/"; \
	  elif [ -f "$$src" ]; then \
	    case "$$dst" in */) out=$$stage/$$dst$$(basename "$$src") ;; *) out=$$stage/$$dst ;; esac; \
	    if [ -d "$$out" ]; then \
	      echo "SDIMAGE_FILES: $$p: $$dst is a directory, end it with / to copy into it" >&2; exit 1; fi; \
	    echo "cp $$src $$out"; mkdir -p "$$(dirname "$$out")"; cp "$$src" "$$out"; \
	  else \
	    echo "SDIMAGE_FILES: $$p: $$src is missing" >&2; exit 1; \
	  fi; \
	done
	$(MAKE) --no-print-directory -f $(firstword $(MAKEFILE_LIST)) sdimage-stage
	$(ROBCO_CORE)/tools/mksdimage.sh $(BUILD)/sd.img $(SDIMAGE_MIB) $(SDIMAGE_STAGE)

# The hook. Empty here: each project rule adds a recipe.
sdimage-stage::

-include $(ROBCO_OBJS:.o=.d) $(PROJ_OBJS:.o=.d)
