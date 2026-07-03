PPB_ROOT := ..
BUILD := build

.PHONY: help clean lib
help:
	@echo "differential targets: reflection-diff, fuzz-sink-replay, structured-replay, conformance*, ci"

# Build the PPB static library at the repo root.
lib:
	$(MAKE) -C $(PPB_ROOT) build/libppb.a

$(PPB_ROOT)/build/libppb.a:
	$(MAKE) -C $(PPB_ROOT) build/libppb.a

clean:
	rm -rf $(BUILD)
