PYTHON ?= python3
VENV := .venv
SPIKES := s1-uart-hello s2-sd-read s3-fb-mmu
MACHINES := raspi0 raspi1ap raspi2b raspi3b

.PHONY: venv test test-canary test-spikes test-examples test-target

venv: $(VENV)/.installed

$(VENV)/.installed: tests/requirements.txt
	$(PYTHON) -m venv $(VENV)
	$(VENV)/bin/pip install -q -r tests/requirements.txt
	touch $@

test: test-canary test-spikes test-examples test-target

test-canary: venv
	$(VENV)/bin/pytest tests/canary -v

# Stops at the first failure and names the spike and machine.
test-spikes: venv
	@set -eu; \
	fail() { echo "FAIL: $$1" >&2; exit 1; }; \
	for s in $(SPIKES); do \
	  $(MAKE) -C spikes/$$s || fail "$$s build"; \
	  for m in $(MACHINES); do \
	    spikes/$$s/run-qemu.sh $$m || fail "$$s on $$m"; \
	  done; \
	done; \
	spikes/s2-sd-read/run-qemu.sh raspi2b --sdhc || fail "s2-sd-read --sdhc on raspi2b"; \
	for m in raspi0 raspi2b; do \
	  $(VENV)/bin/python spikes/s3-fb-mmu/check_screen.py $$m || fail "s3-fb-mmu check_screen on $$m"; \
	done; \
	echo "test-spikes: all passed"

# Builds the examples warning-free. test-target boots them.
test-examples:
	@set -eu; \
	fail() { echo "FAIL: $$1" >&2; exit 1; }; \
	$(MAKE) -C examples/hello CI=1 || fail "hello build"; \
	$(MAKE) -C examples/hello CI=1 BUILD=build/fault EXTRA_CFLAGS=-DHELLO_FAULT=1 || fail "hello fault build"; \
	echo "test-examples: all passed"

# Target tests: boot spikes and examples through tests/harness/boot.py on every board.
test-target: venv
	$(VENV)/bin/pytest tests/target -v --board all
