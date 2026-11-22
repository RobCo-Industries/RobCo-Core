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
# tests/tools, the read-only file system test, which links a second copy
# of the file system built with ROBCO_FS_READONLY=1.
#
# Options: ROBCO_CORE (default: the current directory), HOST_CC (default
# cc), HOST_BUILD (default $(BUILD)/host, or build/host), HOST_SANITIZE=0
# turns sanitizers off, HOST_SANITIZE=require fails when the compiler
# cannot use them.

ROBCO_CORE    ?= .
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

# Probe once: some compilers or platforms lack ASan or UBSan.
ifneq ($(filter 1 require,$(HOST_SANITIZE)),)
HOST_SAN_FLAGS := -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
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
HOST_ALL += $(HOST_BUILD)/fs_tool $(HOST_BUILD)/test_fs_readonly $(HOST_RO_IMG)
HOST_RUN += $(HOST_BUILD)/test_fs_readonly
endif

.PHONY: test-host host-tests

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
endif

# Runs every binary, then fails if any failed.
test-host: $(HOST_ALL)
	@set -u; failed=""; \
	echo "test-host: sanitizers $(if $(filter 1,$(HOST_SAN_OK)),on,off)"; \
	for t in $(HOST_RUN); do \
	  $$t || failed="$$failed $${t##*/}"; \
	done; \
	if [ -n "$$failed" ]; then echo "test-host: FAILED:$$failed" >&2; exit 1; fi; \
	echo "test-host: all passed"
