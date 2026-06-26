UV ?= uv

.PHONY: all check test format format-check lint typecheck regen

all: check

# Static checks, then tests.
check: format-check lint typecheck test

test:
	$(UV) run pytest -q

# Rewrite sources in place (ruff's formatter).
format:
	$(UV) run ruff format .

# Fail if anything is not already formatted (for CI / check)
format-check:
	$(UV) run ruff format --check .

lint:
	$(UV) run ruff check .

typecheck:
	$(UV) run pyrefly check

# Regenerate the committed golden headers and invalid-input goldens.
regen:
	./regen_golden.sh
	./regen_invalid.py
