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

.PHONY: reflection-random-diff
# Randomized round-trip differential over the reflection sink.  Fixed per-message
# seeds keep it deterministic for CI.  Reuses the headers reflection-diff put in
# $(GENDIR).
reflection-random-diff: reflection-diff
	@if pkg-config --exists protobuf 2>/dev/null; then \
	  set -e; \
	  $(CXX) $(CXXFLAGS) -I. -I$(GENDIR) -I$(PPB_ROOT)/include $(PB_CFLAGS) -o $(BUILD)/reflection_random_diff \
	    $(REFLECT_PB_CC) \
	    reflection_random_diff.cc $(PPB_ROOT)/build/libppb.a $(PB_LIBS); \
	  ./$(BUILD)/reflection_random_diff && echo "libprotobuf_random_diff OK"; \
	else \
	  echo "reflection-random-diff: libprotobuf not found, skipping"; \
	fi

# Reflection-sink byte-fuzzer: pipe raw fuzzer bytes through both libprotobuf's
# parser and the PPB sink, and assert they agree on validity and on parsed value.
FUZZ_CXX ?= clang++
FUZZ_SAN := -g -fsanitize=address,undefined -fno-sanitize-recover=all
FUZZ_SINK_PROTOS := pbscalars3.proto pbcomposite3.proto
FUZZ_SINK_OPT := mode=full,always_dispatch_strings
REFL_STRICT := $(BUILD)/refl_strict
FUZZ_CORPUS := $(BUILD)/fuzz_corpus

$(BUILD)/fuzz_sink_bytes: fuzz/fuzz_sink_bytes.cc fuzz/reflection_differential.hpp $(PPB_ROOT)/build/libppb.a
	@mkdir -p $(REFL_STRICT)
	protoc --proto_path=$(PROTODATA) --cpp_out=$(REFL_STRICT) $(FUZZ_SINK_PROTOS)
	protoc $(PROD_PLUGIN) --proto_path=$(PROTODATA) --ppb_opt=$(FUZZ_SINK_OPT) --ppb_out=$(REFL_STRICT) $(FUZZ_SINK_PROTOS)
	protoc $(REFLECT_PLUGIN) --proto_path=$(PROTODATA) --ppb_opt=$(FUZZ_SINK_OPT) --ppb_out=$(REFL_STRICT) $(FUZZ_SINK_PROTOS)
	$(FUZZ_CXX) $(CXXFLAGS) $(FUZZ_SAN) -fsanitize=fuzzer -I. -Ifuzz -I$(REFL_STRICT) -I$(PPB_ROOT)/include $(PB_CFLAGS) \
	  -o $@ fuzz/fuzz_sink_bytes.cc $(REFL_STRICT)/pbscalars3.pb.cc $(REFL_STRICT)/pbcomposite3.pb.cc \
	  $(PPB_ROOT)/build/libppb.a $(PB_LIBS)

.PHONY: fuzz-sink-build fuzz-sink-replay fuzz-sink
fuzz-sink-build:
	@if pkg-config --exists protobuf 2>/dev/null; then $(MAKE) $(BUILD)/fuzz_sink_bytes; \
	else echo "fuzz-sink-build: libprotobuf not found, skipping"; fi

# Deterministic replay: generate the seed corpus with the shared, stdlib-only
# gen_corpus.py, then execute every seed once and exit.
fuzz-sink-replay: fuzz-sink-build
	@if [ -x $(BUILD)/fuzz_sink_bytes ]; then \
	  python3 $(PPB_ROOT)/fuzz/gen_corpus.py $(FUZZ_CORPUS) >/dev/null; \
	  ./$(BUILD)/fuzz_sink_bytes -runs=0 $(FUZZ_CORPUS) && echo "fuzz_sink_bytes replay OK"; \
	else \
	  echo "fuzz-sink-replay: skipped"; \
	fi

# Local-only actual fuzzing (NOT in ci).
FUZZ_TIME ?= 60
fuzz-sink: fuzz-sink-build
	@python3 $(PPB_ROOT)/fuzz/gen_corpus.py $(FUZZ_CORPUS) >/dev/null
	./$(BUILD)/fuzz_sink_bytes -max_total_time=$(FUZZ_TIME) -max_len=1024 $(FUZZ_CORPUS)

ci: test-utf8 reflection-diff reflection-random-diff fuzz-sink-replay
	@echo "differential ci: deterministic subset OK"
