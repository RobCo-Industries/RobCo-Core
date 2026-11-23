# Host unit tests, built with the host compiler and run on the host.
#
# Rule: src/lib/ holds hardware-independent code that builds and runs on
# the host. Code that touches registers or drivers goes in src/drv/,
# src/arch/, src/board/ or src/gfx/.
#
# RobCo-Core's own Makefile includes this file. A project includes it too,
# after mk/robco.mk:
#
#   HOST_TEST_DIR := tests/host        (the project's test_<name>.c files)
#   HOST_SRCS     := kernel/settings.c (pure project sources, linked into every test)
#   HOST_INCLUDES := kernel            (extra include directories)
#   HOST_DEFINES  := FOO=1             (extra -D defines)
#   HOST_CORE_SETS := fs fatfs         (optional core sets, see below)
#   include $(ROBCO_CORE)/mk/host.mk
#
# Every test links the pure library, $(ROBCO_CORE)/src/lib/*.c, and
# HOST_SRCS. HOST_CORE_SETS adds "fs" (src/fs and the test block devices
# in tests/host/blk_file.c) and "fatfs" (third_party/fatfs). "fs" needs
# "fatfs". Tests include rc_test.h from $(ROBCO_CORE)/tests/host.
#
# Without HOST_TEST_DIR the file builds RobCo-Core's own tests: every
# tests/host/test_<name>.c with the file system, build/host/fs_tool for
# tests/tools, build/host/tft_tool for tests/target/test_tft.py,
# the read-only file system test, which links a second copy
# of the file system built with ROBCO_FS_READONLY=1, and the images
# tools/rimg.py packs for test_rimg_pack.
#
# Options: ROBCO_CORE (default: the current directory), HOST_CC (default
# cc), HOST_BUILD (default $(BUILD)/host, or build/host), HOST_SANITIZE=0
# turns sanitizers off, HOST_SANITIZE=require fails when the compiler
# cannot use them.
#
# test-host-gccwarn, run by test-host, compiles every C file in
# HOST_TEST_DIR, every source the tests link and HOST_GCCWARN_SRCS with the
# cross GCC and the host headers, at CI's flags and at -O2, to catch the
# GCC-only warnings CI reports when HOST_CC is clang. It skips, with one
# line, when HOST_CC is GCC, the cross GCC is missing or no host headers
# are found. HOST_GCCWARN_CC (default $(CROSS)gcc) and HOST_GCCWARN_INC
# (default: the macOS SDK's usr/include) override the tools.

ROBCO_CORE    ?= .
PYTHON        ?= python3
HOST_CC       ?= cc
HOST_SANITIZE ?= 1
HOST_BUILD    ?= $(or $(BUILD),build)/host

# "" for the RobCo-Core root itself, so its paths stay as before.
ROBCO_HOST_P := $(if $(filter .,$(ROBCO_CORE)),,$(ROBCO_CORE)/)

ifeq ($(strip $(HOST_TEST_DIR)),)
ROBCO_HOST_SELF := 1
HOST_TEST_DIR   := $(ROBCO_HOST_P)tests/host
HOST_CORE_SETS  ?= fs fatfs
endif

ifneq ($(filter fs,$(HOST_CORE_SETS)),)
ifeq ($(filter fatfs,$(HOST_CORE_SETS)),)
$(error HOST_CORE_SETS: fs needs fatfs)
endif
endif

HOST_CFLAGS := -std=gnu11 -Wall -Wextra -Werror -g -O1 -DROBCO_HOST=1 \
               -I $(ROBCO_HOST_P)include -I $(ROBCO_HOST_P)tests/host \
               $(addprefix -I ,$(HOST_INCLUDES)) $(addprefix -D,$(HOST_DEFINES))

# CI builds with these. Probe once: some compilers or platforms lack ASan or UBSan.
HOST_SAN_FLAGS := -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
ifneq ($(filter 1 require,$(HOST_SANITIZE)),)
HOST_SAN_OK := $(shell mkdir -p $(HOST_BUILD) && \
                 printf 'int main(void){return 0;}\n' > $(HOST_BUILD)/probe.c && \
                 $(HOST_CC) $(HOST_SAN_FLAGS) -o $(HOST_BUILD)/probe $(HOST_BUILD)/probe.c >/dev/null 2>&1 && \
                 $(HOST_BUILD)/probe && echo 1; rm -rf $(HOST_BUILD)/probe $(HOST_BUILD)/probe.c $(HOST_BUILD)/probe.dSYM)
ifeq ($(HOST_SAN_OK),1)
HOST_CFLAGS += $(HOST_SAN_FLAGS)
else ifeq ($(HOST_SANITIZE),require)
$(error $(HOST_CC) cannot build and run with $(HOST_SAN_FLAGS))
endif
endif

HOST_LIB_SRCS := $(sort $(wildcard $(ROBCO_HOST_P)src/lib/*.c))
HOST_RO_TEST := $(HOST_TEST_DIR)/test_fs_readonly.c
HOST_TEST_SRCS := $(sort $(wildcard $(HOST_TEST_DIR)/test_*.c))
ifeq ($(ROBCO_HOST_SELF),1)
HOST_TEST_SRCS := $(filter-out $(HOST_RO_TEST),$(HOST_TEST_SRCS))
endif
HOST_TESTS := $(patsubst $(HOST_TEST_DIR)/%.c,$(HOST_BUILD)/%,$(HOST_TEST_SRCS))
HOST_HDRS := $(ROBCO_HOST_P)tests/host/rc_test.h $(ROBCO_HOST_P)tests/host/blk_file.h \
             $(wildcard $(ROBCO_HOST_P)include/robco/*.h) $(wildcard $(HOST_TEST_DIR)/*.h) \
             $(foreach d,$(HOST_INCLUDES),$(wildcard $(d)/*.h))

# src/fs and FatFs are objects built once. Only they see ff.h and our
# src/fs/ffconf.h. HOST_FATFS_CFLAGS relaxes warnings for FatFs files only.
HOST_FS_CFLAGS := -I $(ROBCO_HOST_P)src/fs -I $(ROBCO_HOST_P)third_party/fatfs
HOST_FATFS_CFLAGS :=
HOST_FS_HDRS := $(wildcard $(ROBCO_HOST_P)src/fs/*.h) $(wildcard $(ROBCO_HOST_P)third_party/fatfs/*.h)
HOST_FS_OBJS := $(if $(filter fs,$(HOST_CORE_SETS)),\
                  $(patsubst $(ROBCO_HOST_P)src/fs/%.c,$(HOST_BUILD)/fs/%.o,$(sort $(wildcard $(ROBCO_HOST_P)src/fs/*.c))) \
                  $(ROBCO_HOST_P)tests/host/blk_file.c)
HOST_FATFS_OBJS := $(if $(filter fatfs,$(HOST_CORE_SETS)),\
                     $(HOST_BUILD)/fatfs/ff.o $(HOST_BUILD)/fatfs/ffunicode.o)
HOST_LINK := $(HOST_LIB_SRCS) $(HOST_SRCS) $(HOST_FS_OBJS) $(HOST_FATFS_OBJS)
HOST_RO_FS_OBJS := $(subst $(HOST_BUILD)/,$(HOST_BUILD)/ro/,$(HOST_FS_OBJS) $(HOST_FATFS_OBJS))
HOST_RO_LINK := $(HOST_LIB_SRCS) $(HOST_RO_FS_OBJS)
HOST_RO_IMG := $(HOST_BUILD)/fs_readonly.img

# What test-host builds, and what it runs.
HOST_ALL := $(HOST_TESTS)
HOST_RUN := $(HOST_TESTS)
ifeq ($(ROBCO_HOST_SELF),1)
HOST_RIMG_DIR := $(HOST_BUILD)/rimg
HOST_CFLAGS += -DROBCO_HOST_RIMG_DIR='"$(HOST_RIMG_DIR)"'
HOST_RIMG_IMGS := $(HOST_RIMG_DIR)/rom1.img $(HOST_RIMG_DIR)/holo.img $(HOST_RIMG_DIR)/boot.img
HOST_ALL += $(HOST_BUILD)/fs_tool $(HOST_BUILD)/tft_tool $(HOST_BUILD)/test_fs_readonly $(HOST_RO_IMG) $(HOST_RIMG_IMGS)
HOST_RUN += $(HOST_BUILD)/test_fs_readonly
endif

.PHONY: test-host host-tests test-host-gccwarn

# Keep the objects: only pattern rules name them in a project build.
.SECONDARY: $(filter %.o,$(HOST_FS_OBJS) $(HOST_FATFS_OBJS) $(HOST_RO_FS_OBJS))

host-tests: $(HOST_ALL)

$(HOST_BUILD)/fs/%.o: $(ROBCO_HOST_P)src/fs/%.c $(HOST_FS_HDRS) $(wildcard $(ROBCO_HOST_P)include/robco/*.h)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) $(HOST_FS_CFLAGS) -c -o $@ $<

$(HOST_BUILD)/fatfs/%.o: $(ROBCO_HOST_P)third_party/fatfs/%.c $(HOST_FS_HDRS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) $(HOST_FS_CFLAGS) $(HOST_FATFS_CFLAGS) -c -o $@ $<

$(HOST_BUILD)/test_%: $(HOST_TEST_DIR)/test_%.c $(HOST_LINK) $(HOST_HDRS)
	@mkdir -p $(HOST_BUILD)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $< $(HOST_LINK)

ifeq ($(ROBCO_HOST_SELF),1)
# The read-only flavour: the same sources with ROBCO_FS_READONLY=1.
$(HOST_BUILD)/ro/fs/%.o: src/fs/%.c $(HOST_FS_HDRS) $(wildcard include/robco/*.h)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) -DROBCO_FS_READONLY=1 $(HOST_FS_CFLAGS) -c -o $@ $<

$(HOST_BUILD)/ro/fatfs/%.o: third_party/fatfs/%.c $(HOST_FS_HDRS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) -DROBCO_FS_READONLY=1 $(HOST_FS_CFLAGS) $(HOST_FATFS_CFLAGS) -c -o $@ $<

$(HOST_BUILD)/test_fs_readonly: $(HOST_RO_TEST) $(HOST_RO_LINK) $(HOST_HDRS)
	@mkdir -p $(HOST_BUILD)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $< $(HOST_RO_LINK)

# A 1 MiB FAT volume written by the read-write fs_tool for the read-only test.
$(HOST_RO_IMG): $(HOST_BUILD)/fs_tool tests/host/rc_test.h
	rm -f $@
	dd if=/dev/zero of=$@.tmp bs=1024 count=1024 2>/dev/null
	$(HOST_BUILD)/fs_tool $@.tmp mkfs fat
	$(HOST_BUILD)/fs_tool $@.tmp mkdir A
	$(HOST_BUILD)/fs_tool $@.tmp put tests/host/rc_test.h A/B.TXT
	mv $@.tmp $@

# Host CLI over the fs API, used by tests/tools/test_fs_interop.py.
$(HOST_BUILD)/fs_tool: tests/host/fs_tool.c $(HOST_LINK) $(HOST_HDRS)
	@mkdir -p $(HOST_BUILD)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $< $(HOST_LINK)

# Images packed by tools/rimg.py for tests/host/test_rimg_pack.c, which
# runs from the root and reads them from $(HOST_RIMG_DIR).
HOST_RIMG_PAYLOAD := $(HOST_RIMG_DIR)/payload.bin
$(HOST_RIMG_PAYLOAD):
	@mkdir -p $(dir $@)
	printf 'payload bytes for the rimg pack round trip\n' > $@

$(HOST_RIMG_DIR)/rom1.img: $(HOST_RIMG_PAYLOAD) tools/rimg.py tests/host/icon.pbm
	$(PYTHON) tools/rimg.py pack $< -o $@ --magic ROM1 --subtype 0 --load-addr 0x400000 \
	    --name DEITRIX --image-version 3.0.3 --title "Deitrix test ROM" --icon tests/host/icon.pbm
	$(PYTHON) tools/rimg.py verify $@ >/dev/null

$(HOST_RIMG_DIR)/holo.img: $(HOST_RIMG_PAYLOAD) tools/rimg.py
	$(PYTHON) tools/rimg.py pack $< -o $@ --magic ROBCO78 --subtype 1 --load-addr 0x800000 \
	    --name HOLO --mem-size 0x10000
	$(PYTHON) tools/rimg.py verify $@ >/dev/null

$(HOST_RIMG_DIR)/boot.img: $(HOST_RIMG_PAYLOAD) tools/rimg.py
	$(PYTHON) tools/rimg.py pack $< -o $@ --magic BOOT --subtype 7 --load-addr 0x8000
	$(PYTHON) tools/rimg.py verify $@ >/dev/null

$(HOST_BUILD)/test_rimg_pack: $(HOST_RIMG_IMGS)

# test_video builds the framebuffer and console drivers in with #include.
$(HOST_BUILD)/test_video: src/drv/fb.c src/gfx/console.c src/gfx/schemes.c src/gfx/font8x16_fixedsys.c
# test_audio_stop builds the audio driver and its trace backend in the same way.
$(HOST_BUILD)/test_audio_stop: src/drv/audio.c src/drv/audio_trace.c

# These include the ILI9341 driver through tests/host/tft_fake.h.
$(HOST_BUILD)/test_tft_bus $(HOST_BUILD)/test_tft_panel: src/drv/tft_ili9341.c

# Expected bus CRC of examples/tft, used by tests/target/test_tft.py.
$(HOST_BUILD)/tft_tool: tests/host/tft_tool.c src/drv/tft_ili9341.c examples/tft/picture.h $(HOST_LINK) $(HOST_HDRS)
	@mkdir -p $(HOST_BUILD)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $< $(HOST_LINK)

# test_gpiox.c includes the driver source to run it on a fake bus.
$(HOST_BUILD)/test_gpiox: src/drv/gpiox_pca9554.c
endif

# GCC warnings without a host GCC: compile only, never link, with the
# cross GCC and the host headers. Each file is compiled twice:
#   ci: CI's host flags, the sanitizers and -O1, minus -g. UBSan hides
#       loop bounds from GCC, so some -Wformat-truncation warnings fire
#       only with them.
#   o2: the same flags without the sanitizers, at -O2, which other GCC
#       warnings need.
# The cross GCC targets 32-bit ARM, so pointer and integer casts of
# different sizes are not checked: on a 64-bit host they are the same
# size, and clang checks them there.
#
# Sources: every .c file in HOST_TEST_DIR, the pure library, HOST_SRCS,
# the file system sets in HOST_CORE_SETS, and HOST_GCCWARN_SRCS, extra
# project files such as host tools kept elsewhere. Each file and pass
# leaves a stamp in $(HOST_BUILD)/gccwarn, so a second run compiles only
# what changed, and make -j runs the compiles in parallel.
HOST_GCCWARN_CC ?= $(or $(CROSS),arm-none-eabi-)gcc
HOST_GCCWARN_INC ?=
HOST_GCCWARN_SRCS ?=

# "ok <include dir>" or "skip <reason>", found once when make starts.
HOST_GW_STATE := $(shell \
  defs=$$($(HOST_CC) -dM -E -x c /dev/null 2>/dev/null); \
  case "$$defs" in (*__clang__*) ;; (*__GNUC__*) echo "skip $(HOST_CC) is GCC"; exit 0;; esac; \
  command -v $(HOST_GCCWARN_CC) >/dev/null 2>&1 || { echo "skip no $(HOST_GCCWARN_CC)"; exit 0; }; \
  inc="$(HOST_GCCWARN_INC)"; \
  if [ -z "$$inc" ] && command -v xcrun >/dev/null 2>&1; then \
    inc="$$(xcrun --show-sdk-path 2>/dev/null)/usr/include"; \
  fi; \
  if [ -n "$$inc" ] && [ -f "$$inc/stdio.h" ]; then echo "ok $$inc"; \
  else echo "skip no host headers, set HOST_GCCWARN_INC"; fi)
HOST_GW_INC := $(wordlist 2,$(words $(HOST_GW_STATE)),$(HOST_GW_STATE))

HOST_GW_OUT := $(HOST_BUILD)/gccwarn
HOST_GW_CC = $(HOST_GCCWARN_CC) -c -o /dev/null -MMD -MT $@ -MF $(@:.ok=.d) \
             -Werror -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -idirafter "$(HOST_GW_INC)"
HOST_GW_FLAGS_ci = $(filter-out -g $(HOST_SAN_FLAGS),$(HOST_CFLAGS)) $(HOST_SAN_FLAGS)
HOST_GW_FLAGS_o2 = $(filter-out -g -O1 $(HOST_SAN_FLAGS),$(HOST_CFLAGS)) -O2
HOST_GW_GROUP_src :=
HOST_GW_GROUP_fs = $(HOST_FS_CFLAGS)
HOST_GW_GROUP_fatfs = $(HOST_FS_CFLAGS) $(HOST_FATFS_CFLAGS)
HOST_GW_GROUP_ro = -DROBCO_FS_READONLY=1 $(HOST_FS_CFLAGS) $(HOST_FATFS_CFLAGS)

HOST_GW_SRCS_src := $(sort $(wildcard $(HOST_TEST_DIR)/*.c) \
                      $(filter %.c,$(HOST_LIB_SRCS) $(HOST_SRCS) $(HOST_GCCWARN_SRCS)) \
                      $(if $(filter fs,$(HOST_CORE_SETS)),$(ROBCO_HOST_P)tests/host/blk_file.c))
HOST_GW_SRCS_fs := $(if $(filter fs,$(HOST_CORE_SETS)),$(sort $(wildcard $(ROBCO_HOST_P)src/fs/*.c)))
HOST_GW_SRCS_fatfs := $(if $(filter fatfs,$(HOST_CORE_SETS)),\
                        $(ROBCO_HOST_P)third_party/fatfs/ff.c $(ROBCO_HOST_P)third_party/fatfs/ffunicode.c)
HOST_GW_SRCS_ro := $(if $(ROBCO_HOST_SELF),$(HOST_GW_SRCS_fs) $(HOST_GW_SRCS_fatfs))

# One static pattern rule per pass and group:
# $(HOST_BUILD)/gccwarn/<pass>/<group>/<source>.ok
define HOST_GW_RULE
HOST_GW_STAMPS += $$(patsubst %,$$(HOST_GW_OUT)/$(1)/$(2)/%.ok,$$(HOST_GW_SRCS_$(2)))
$$(patsubst %,$$(HOST_GW_OUT)/$(1)/$(2)/%.ok,$$(HOST_GW_SRCS_$(2))): $$(HOST_GW_OUT)/$(1)/$(2)/%.ok: % $$(ROBCO_HOST_P)mk/host.mk | $$(HOST_ALL)
	@mkdir -p $$(dir $$@)
	@$$(HOST_GW_CC) $$(HOST_GW_FLAGS_$(1)) $$(HOST_GW_GROUP_$(2)) $$< || \
	  { echo "test-host-gccwarn: $$< fails the $(1) pass" >&2; exit 1; }
	@touch $$@
endef
HOST_GW_STAMPS :=
$(foreach p,ci o2,$(foreach g,src fs fatfs ro,$(eval $(call HOST_GW_RULE,$(p),$(g)))))
-include $(HOST_GW_STAMPS:.ok=.d)

# RobCo-Core's self-test: a file that is bad on purpose must fail the ci
# pass. It passes the o2 pass, which is the hole the ci pass closes.
ifeq ($(ROBCO_HOST_SELF),1)
HOST_GW_BAD := tests/fixtures/gccwarn/format_truncation_bad.c
HOST_GW_SELF := $(HOST_GW_OUT)/selftest.ok
$(HOST_GW_SELF): $(HOST_GW_BAD) mk/host.mk
	@mkdir -p $(dir $@)
	@if $(HOST_GW_CC) $(HOST_GW_FLAGS_ci) $< 2>/dev/null; then \
	  echo "test-host-gccwarn: self-test: the ci pass missed $<" >&2; exit 1; fi
	@$(HOST_GW_CC) $(HOST_GW_FLAGS_o2) $< || \
	  { echo "test-host-gccwarn: self-test: the o2 pass should accept $<" >&2; exit 1; }
	@touch $@
endif

ifeq ($(firstword $(HOST_GW_STATE)),ok)
test-host-gccwarn: $(HOST_ALL) $(HOST_GW_SELF) $(HOST_GW_STAMPS)
	@echo "test-host-gccwarn: $(HOST_GCCWARN_CC) found no warning in the ci and o2 passes"
else
test-host-gccwarn: $(HOST_ALL)
	@echo "test-host-gccwarn: skipped, $(HOST_GW_INC)"
endif

# Runs every binary, then fails if any failed.
test-host: test-host-gccwarn $(HOST_ALL)
	@set -u; failed=""; \
	echo "test-host: sanitizers $(if $(filter 1,$(HOST_SAN_OK)),on,off)"; \
	for t in $(HOST_RUN); do \
	  $$t || failed="$$failed $${t##*/}"; \
	done; \
	if [ -n "$$failed" ]; then echo "test-host: FAILED:$$failed" >&2; exit 1; fi; \
	echo "test-host: all passed"

