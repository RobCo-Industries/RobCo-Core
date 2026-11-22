# Host unit tests. Included by the top-level Makefile.
#
# Rule: src/lib/ holds hardware-independent code that builds and runs on
# the host. Code that touches registers or drivers goes in src/drv/,
# src/arch/, src/board/ or src/gfx/.
#
# Builds build/host/test_<name> from tests/host/test_<name>.c, every
# src/lib/*.c, the file system layer (src/fs/*.c and FatFs) and the test
# block devices in tests/host/blk_file.c with the host compiler, then runs
# each binary. Also builds build/host/fs_tool for tests/tools.
#
# Options: HOST_CC (default cc), HOST_SANITIZE=0 turns sanitizers off,
# HOST_SANITIZE=require fails when the compiler cannot use them.

HOST_CC       ?= cc
HOST_SANITIZE ?= 1
HOST_BUILD    := build/host

HOST_CFLAGS := -std=gnu11 -Wall -Wextra -Werror -g -O1 -DROBCO_HOST=1 \
               -I include -I tests/host

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

HOST_LIB_SRCS := $(sort $(wildcard src/lib/*.c))
HOST_TEST_SRCS := $(sort $(wildcard tests/host/test_*.c))
HOST_TESTS := $(patsubst tests/host/%.c,$(HOST_BUILD)/%,$(HOST_TEST_SRCS))
HOST_HDRS := tests/host/rc_test.h tests/host/blk_file.h $(wildcard include/robco/*.h)

# src/fs and FatFs are objects built once. Only they see ff.h and our
# src/fs/ffconf.h. HOST_FATFS_CFLAGS relaxes warnings for FatFs files only.
HOST_FS_CFLAGS := -I src/fs -I third_party/fatfs
HOST_FATFS_CFLAGS :=
HOST_FS_HDRS := $(wildcard src/fs/*.h) $(wildcard third_party/fatfs/*.h)
HOST_FS_OBJS := $(patsubst src/fs/%.c,$(HOST_BUILD)/fs/%.o,$(sort $(wildcard src/fs/*.c))) \
                $(HOST_BUILD)/fatfs/ff.o $(HOST_BUILD)/fatfs/ffunicode.o
HOST_LINK := $(HOST_LIB_SRCS) tests/host/blk_file.c $(HOST_FS_OBJS)

.PHONY: test-host host-tests

host-tests: $(HOST_TESTS) $(HOST_BUILD)/fs_tool

$(HOST_BUILD)/fs/%.o: src/fs/%.c $(HOST_FS_HDRS) $(wildcard include/robco/*.h)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) $(HOST_FS_CFLAGS) -c -o $@ $<

$(HOST_BUILD)/fatfs/%.o: third_party/fatfs/%.c $(HOST_FS_HDRS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) $(HOST_FS_CFLAGS) $(HOST_FATFS_CFLAGS) -c -o $@ $<

$(HOST_BUILD)/test_%: tests/host/test_%.c $(HOST_LINK) $(HOST_HDRS)
	@mkdir -p $(HOST_BUILD)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $< $(HOST_LINK)

# Host CLI over the fs API, used by tests/tools/test_fs_interop.py.
$(HOST_BUILD)/fs_tool: tests/host/fs_tool.c $(HOST_LINK) $(HOST_HDRS)
	@mkdir -p $(HOST_BUILD)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $< $(HOST_LINK)

# Runs every binary, then fails if any failed.
test-host: $(HOST_TESTS) $(HOST_BUILD)/fs_tool
	@set -u; failed=""; \
	echo "test-host: sanitizers $(if $(filter 1,$(HOST_SAN_OK)),on,off)"; \
	for t in $(HOST_TESTS); do \
	  ./$$t || failed="$$failed $${t##*/}"; \
	done; \
	if [ -n "$$failed" ]; then echo "test-host: FAILED:$$failed" >&2; exit 1; fi; \
	echo "test-host: all passed"
