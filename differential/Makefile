PPB_ROOT := ..
BUILD := build

CXX ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra

.PHONY: help ci clean lib
help:
	@echo "differential targets: reflection-diff, fuzz-sink-replay, structured-replay, conformance*, ci"

# Build the PPB static library at the repo root.
lib:
	$(MAKE) -C $(PPB_ROOT) build/libppb.a

$(PPB_ROOT)/build/libppb.a:
	$(MAKE) -C $(PPB_ROOT) build/libppb.a

clean:
	rm -rf $(BUILD)

.PHONY: test-utf8
test-utf8:
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -I. -o $(BUILD)/test_utf8 test_utf8.cc
	./$(BUILD)/test_utf8 && echo "test_utf8 OK"

# Deterministic subset run in CI (no Docker, no time-bounded fuzzing).
ci: test-utf8
	@echo "differential ci: deterministic subset OK"
