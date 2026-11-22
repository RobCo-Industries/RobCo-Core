PYTHON ?= python3
VENV := .venv

.PHONY: venv test-canary

venv: $(VENV)/.installed

$(VENV)/.installed: tests/requirements.txt
	$(PYTHON) -m venv $(VENV)
	$(VENV)/bin/pip install -q -r tests/requirements.txt
	touch $@

test-canary: venv
	$(VENV)/bin/pytest tests/canary -v
