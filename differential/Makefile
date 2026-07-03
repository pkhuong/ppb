PPB_ROOT := ..
GEN := $(PPB_ROOT)/generator
PROTODATA := $(GEN)/testdata
REFLECT := protoc_gen_ppb_reflect.py
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

PROD_PLUGIN := --plugin=protoc-gen-ppb=$(GEN)/protoc_gen_ppb.py  # production schema -> foo.ppb.hpp
REFLECT_PLUGIN := --plugin=protoc-gen-ppb=$(REFLECT)             # reflect sink -> foo.ppb.reflect.hpp
PB_CFLAGS := $$(pkg-config --cflags protobuf)
PB_LIBS := $$(pkg-config --libs protobuf)
GENDIR := $(BUILD)/gen
comma := ,

# The proto fixtures both reflection differentials build, and the libprotobuf
# .pb.cc translation units derived from them.
REFLECT_PROTOS := pbscalars3.proto nested2.proto pbcomposite3.proto pbcomposite2.proto pbrecursive.proto pbmrec.proto pbrrec.proto oneof3.proto empty3.proto wkt_user3.proto
REFLECT_PB_CC := $(patsubst %.proto,$(GENDIR)/%.pb.cc,$(REFLECT_PROTOS))

# Generate schema (prod) + sink (reflect) for one --ppb_opt string.
define gen_both
	protoc $(PROD_PLUGIN) --proto_path=$(PROTODATA) --ppb_opt=$(1) --ppb_out=$(GENDIR) $(2); \
	protoc $(REFLECT_PLUGIN) --proto_path=$(PROTODATA) --ppb_opt=$(1) --ppb_out=$(GENDIR) $(2);
endef

.PHONY: reflection-diff
reflection-diff: $(PPB_ROOT)/build/libppb.a
	@if pkg-config --exists protobuf 2>/dev/null; then \
	  set -e; mkdir -p $(GENDIR); \
	  protoc --proto_path=$(PROTODATA) --cpp_out=$(GENDIR) \
	    $(REFLECT_PROTOS); \
	  $(call gen_both,mode=none,pbscalars3.proto nested2.proto pbcomposite3.proto pbcomposite2.proto empty3.proto wkt_user3.proto) \
	  $(call gen_both,mode=none$(comma)opaque_cycles,pbrecursive.proto pbmrec.proto pbrrec.proto) \
	  $(call gen_both,mode=none$(comma)oneof_as_optional,oneof3.proto) \
	  $(CXX) $(CXXFLAGS) -I. -I$(GENDIR) -I$(PPB_ROOT)/include $(PB_CFLAGS) -o $(BUILD)/reflection_diff \
	    $(REFLECT_PB_CC) \
	    reflection_diff.cc $(PPB_ROOT)/build/libppb.a $(PB_LIBS); \
	  ./$(BUILD)/reflection_diff && echo "libprotobuf_diff OK"; \
	else \
	  echo "reflection-diff: libprotobuf not found, skipping"; \
	fi

# Deterministic subset run in CI (no Docker, no time-bounded fuzzing).
ci: test-utf8 reflection-diff
	@echo "differential ci: deterministic subset OK"
