PYTHON ?= python3
VENV := .venv
SPIKES := s1-uart-hello s2-sd-read s3-fb-mmu
MACHINES := raspi0 raspi1ap raspi2b raspi3b

.PHONY: venv test test-host test-tools test-canary test-spikes test-examples test-target boot-json

venv: $(VENV)/.installed

$(VENV)/.installed: tests/requirements.txt
	$(PYTHON) -m venv $(VENV)
	$(VENV)/bin/pip install -q -r tests/requirements.txt
	touch $@

# test-target rebuilds the spike and example dirs; keep top-level targets serial.
.NOTPARALLEL:

test: test-host test-tools test-docs test-canary test-spikes test-examples test-target

# Unit tests for the Python tools in tools/.
test-tools: venv
	$(VENV)/bin/pytest tests/tools -v

# boot.json from the harness's boot_args(), for tools other than the harness.
# Stdlib only. BOOT_JSON must be an absolute path.
BOOT_JSON ?= $(CURDIR)/build/boot.json
boot-json:
	@case "$(BOOT_JSON)" in /*) ;; *) echo "boot-json: BOOT_JSON must be an absolute path, got '$(BOOT_JSON)'" >&2; exit 1 ;; esac
	mkdir -p $(dir $(BOOT_JSON))
	PYTHONPATH=tests $(PYTHON) -m harness.bootjson -o $(BOOT_JSON)

# Links, API coverage, checked commands and banned words in the Markdown files.
test-docs:
	$(PYTHON) tools/check_docs.py

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

# Builds every examples/*/Makefile warning-free. test-target boots them.
# An example may list extra builds in examples/<name>/VARIANTS, one per
# line: <build-dir> <extra cflags>. Lines starting with # are skipped.
test-examples:
	@set -eu; \
	fail() { echo "FAIL: $$1" >&2; exit 1; }; \
	for mk in examples/*/Makefile; do \
	  d=$${mk%/Makefile}; n=$${d#examples/}; \
	  $(MAKE) -C $$d CI=1 || fail "$$n build"; \
	  [ -f $$d/VARIANTS ] || continue; \
	  while read -r build flags; do \
	    case "$$build" in ''|'#'*) continue ;; esac; \
	    $(MAKE) -C $$d CI=1 BUILD=$$build EXTRA_CFLAGS="$$flags" || fail "$$n $$build build"; \
	  done < $$d/VARIANTS; \
	done; \
	echo "test-examples: all passed"

# Target tests: boot spikes and examples through tests/harness/boot.py.
# BOARD picks one QEMU board, or all of them (default).
BOARD ?= all
test-target: venv
	$(VENV)/bin/pytest tests/target -v -rs --board $(BOARD)

# Host unit tests: src/lib built with the host cc, ASan and UBSan.
include mk/host.mk
