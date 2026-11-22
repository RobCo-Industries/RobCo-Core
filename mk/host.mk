# Host unit tests. Included by the top-level Makefile.
#
# Rule: src/lib/ holds hardware-independent code that builds and runs on
# the host. Code that touches registers or drivers goes in src/drv/,
# src/arch/, src/board/ or src/gfx/.
#
# Builds build/host/test_<name> from tests/host/test_<name>.c and every
# src/lib/*.c with the host compiler, then runs each binary.
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

.PHONY: test-host host-tests

host-tests: $(HOST_TESTS)

$(HOST_BUILD)/test_%: tests/host/test_%.c $(HOST_LIB_SRCS) tests/host/rc_test.h $(wildcard include/robco/*.h)
	@mkdir -p $(HOST_BUILD)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $< $(HOST_LIB_SRCS)

# Runs every binary, then fails if any failed.
test-host: $(HOST_TESTS)
	@set -u; failed=""; \
	echo "test-host: sanitizers $(if $(filter 1,$(HOST_SAN_OK)),on,off)"; \
	for t in $(HOST_TESTS); do \
	  ./$$t || failed="$$failed $${t##*/}"; \
	done; \
	if [ -n "$$failed" ]; then echo "test-host: FAILED:$$failed" >&2; exit 1; fi; \
	echo "test-host: all passed"
