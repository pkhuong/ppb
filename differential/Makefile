PPB_ROOT := ..
GEN := $(PPB_ROOT)/generator
PROTODATA := $(GEN)/testdata
REFLECT := protoc_gen_ppb_reflect.py
BUILD := build

CXX ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra
LPM_TAG ?= v1.5
PROTOBUF_REF ?= v35.0

.PHONY: help ci clean lib
help:
	@echo "Runnable targets (see README.md):"
	@echo "  ci                                     deterministic subset run in CI"
	@echo "  reflection-diff / reflection-random-diff  round-trip differentials"
	@echo "  fuzz-sink-replay / structured-replay   deterministic corpus replay"
	@echo "  fuzz-sink / fuzz-structured            local time-bounded fuzzing"
	@echo "  conformance{,-proto2,-lean}            protobuf conformance suites"
	@echo "  conformance-docker                     conformance in a pinned image"
	@echo "  protobuf-prefix / image                one-time setup"

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
REFL_FULL := $(BUILD)/refl_full
FUZZ_CORPUS := $(BUILD)/fuzz_corpus

$(BUILD)/fuzz_sink_bytes: fuzz/fuzz_sink_bytes.cc fuzz/reflection_differential.hpp $(PPB_ROOT)/build/libppb.a
	@mkdir -p $(REFL_FULL)
	protoc --proto_path=$(PROTODATA) --cpp_out=$(REFL_FULL) $(FUZZ_SINK_PROTOS)
	protoc $(PROD_PLUGIN) --proto_path=$(PROTODATA) --ppb_opt=$(FUZZ_SINK_OPT) --ppb_out=$(REFL_FULL) $(FUZZ_SINK_PROTOS)
	protoc $(REFLECT_PLUGIN) --proto_path=$(PROTODATA) --ppb_opt=$(FUZZ_SINK_OPT) --ppb_out=$(REFL_FULL) $(FUZZ_SINK_PROTOS)
	$(FUZZ_CXX) $(CXXFLAGS) $(FUZZ_SAN) -fsanitize=fuzzer -I. -Ifuzz -I$(REFL_FULL) -I$(PPB_ROOT)/include $(PB_CFLAGS) \
	  -o $@ fuzz/fuzz_sink_bytes.cc $(REFL_FULL)/pbscalars3.pb.cc $(REFL_FULL)/pbcomposite3.pb.cc \
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

# LPM structure-aware fuzzers (fuzz_sink_structured / fuzz_sink_mutate).
# libprotobuf-mutator is not apt-installable, so time-bounded fuzzing runs through the
# pinned Docker image; CI (which builds LPM natively) gets a deterministic
# corpus-replay via structured-replay.  See differential/fuzz/build_and_run.sh.
DOCKER ?= docker
IMAGE_TAG ?= ppb-differential
STRUCTURED_FUZZ_SECONDS ?= 60

# Build/run the image for the host architecture (arm64 on arm64, amd64 on
# x86-64).  Some docker/podman setups otherwise resolve a foreign variant of
# the base image and run it under qemu.
DOCKER_PLATFORM ?= linux/$(shell uname -m | sed -e 's/aarch64/arm64/' -e 's/x86_64/amd64/')

.PHONY: image structured-replay fuzz-structured
image:
	$(DOCKER) image inspect $(IMAGE_TAG) >/dev/null 2>&1 || \
	  $(DOCKER) build --platform=$(DOCKER_PLATFORM) --build-arg LPM_TAG=$(LPM_TAG) --build-arg PROTOBUF_REF=$(PROTOBUF_REF) -t $(IMAGE_TAG) -f Dockerfile $(PPB_ROOT)

# LPM (libprotobuf-mutator) is used from the system install by default, but a
# sibling checkout built to $(LPM_PREFIX) overrides it when present.
LPM_DIR ?= $(PPB_ROOT)/../libprotobuf-mutator
LPM_PREFIX ?= $(LPM_DIR)/_install

# Native deterministic replay.  Prefer a sibling LPM build, else the system
# install, else a clean skip (CI builds LPM system-wide, so it's active there).
structured-replay:
	@if pkg-config --exists protobuf 2>/dev/null && [ -f $(LPM_PREFIX)/lib/libprotobuf-mutator.a ]; then \
	  echo "structured-replay: using sibling LPM at $(abspath $(LPM_PREFIX))"; \
	  REPLAY=1 LPM_INCLUDE="$(abspath $(LPM_PREFIX))/include/libprotobuf-mutator" LPM_LIBDIR="$(abspath $(LPM_PREFIX))/lib" \
	    sh fuzz/build_and_run.sh && echo "structured replay OK"; \
	elif pkg-config --exists protobuf 2>/dev/null && \
	    { [ -f /usr/local/lib/libprotobuf-mutator.a ] || [ -f /usr/lib/libprotobuf-mutator.a ]; }; then \
	  echo "structured-replay: using system LPM"; \
	  REPLAY=1 sh fuzz/build_and_run.sh && echo "structured replay OK"; \
	else \
	  echo "structured-replay: libprotobuf-mutator not present (no sibling $(LPM_PREFIX), no system install), skipping (use 'make -C differential image' + fuzz-structured)"; \
	fi

# randomized fuzzing through the image (NOT in ci).
fuzz-structured: image
	$(DOCKER) run --rm --platform=$(DOCKER_PLATFORM) -v "$(abspath $(PPB_ROOT)):/src:rw,Z" -w /src \
	  -e MAX_TOTAL_TIME=$(STRUCTURED_FUZZ_SECONDS) $(IMAGE_TAG)

# Runs the whole conformance suite inside a container.
#
# Host and in-image runs share the build/ tree; remember to `make clean`.
.PHONY: conformance-docker
conformance-docker: image
	$(DOCKER) run --rm --platform=$(DOCKER_PLATFORM) -v "$(abspath $(PPB_ROOT)):/work/ppb:rw,Z" -w /work/ppb \
	  $(IMAGE_TAG) \
	  make -C differential conformance conformance-proto2 conformance-lean

# Runs the proto3/proto2/lean conformance suites against the reflection sink,
# in a reproducible image.
#
# Use `make conformance-docker` for a simple entry point.
PROTOBUF_DIR ?= $(PPB_ROOT)/../protobuf
PROTOBUF_PREFIX ?= $(PROTOBUF_DIR)/_install
PROTOBUF_SRC ?= $(PROTOBUF_DIR)/src
CONFORMANCE_PROTO_DIR ?= $(PROTOBUF_DIR)/conformance
PROTOC ?= $(PROTOBUF_PREFIX)/bin/protoc
CONFORMANCE_RUNNER ?= $(PROTOBUF_PREFIX)/bin/conformance_test_runner
PROTOBUF_LIBDIR := $(abspath $(PROTOBUF_PREFIX))/lib:$(abspath $(PROTOBUF_PREFIX))/lib64

CONF_PKGCONF := PKG_CONFIG_PATH="$(abspath $(PROTOBUF_PREFIX))/lib/pkgconfig:$(abspath $(PROTOBUF_PREFIX))/lib64/pkgconfig"
CONF_PB_CFLAGS = $$($(CONF_PKGCONF) pkg-config --cflags protobuf)
CONF_PB_LIBS = $$($(CONF_PKGCONF) pkg-config --libs protobuf) -Wl,-rpath,$(PROTOBUF_LIBDIR)

# The full schema inlines every packed/unpacked descriptor of the whole
# closure into one parse_into; on x86-64 the result is so large that it
# needs -mcmodel=large.
CONF_MCMODEL := $(if $(filter x86_64,$(shell uname -m)),-mcmodel=large)
CONFORMANCE_CXXFLAGS := -std=c++20 -O2 $(CONF_MCMODEL)
CONF_GEN := $(BUILD)/conf_gen
CONF_GEN_PROTO2 := $(BUILD)/conf_gen_proto2
CONF_GEN_LEAN := $(BUILD)/conf_gen_lean
CONF_INC := -I. -I$(PPB_ROOT)/include

# The --ppb_opt string for each conformance testing suite
CONF_OPT := mode=full,opaque_cycles,oneof_as_optional,drop_foreign_type_fields,always_dispatch_strings
CONF_OPT_PROTO2 := mode=full,opaque_cycles,oneof_as_optional,drop_group_extension_fields
CONF_OPT_LEAN := mode=lean,opaque_cycles,oneof_as_optional,drop_foreign_type_fields,always_dispatch_strings

have_conformance = [ -x "$(PROTOC)" ] && \
	[ -f $(PROTOBUF_SRC)/google/protobuf/test_messages_proto3.proto ]

.PHONY: protobuf-prefix
protobuf-prefix:
	sh setup_protobuf.sh $(PROTOBUF_DIR) $(PROTOBUF_PREFIX)

.PHONY: conformance conformance-proto2 conformance-lean

# proto3 full: both wire forms + always-on unknown-field
# retention; singular strings routed through the UTF-8-validating sink setter
# via the production always_dispatch_strings hook.
conformance: $(PPB_ROOT)/build/libppb.a
	@if $(have_conformance); then \
	  set -e; mkdir -p $(CONF_GEN); \
	  $(PROTOC) $(PROD_PLUGIN) --proto_path=$(PROTOBUF_SRC) --ppb_opt=$(CONF_OPT) --ppb_out=$(CONF_GEN) google/protobuf/test_messages_proto3.proto; \
	  $(PROTOC) $(REFLECT_PLUGIN) --proto_path=$(PROTOBUF_SRC) --ppb_opt=$(CONF_OPT) --ppb_out=$(CONF_GEN) google/protobuf/test_messages_proto3.proto; \
	  $(PROTOC) --proto_path=$(PROTOBUF_SRC) --cpp_out=$(CONF_GEN) google/protobuf/test_messages_proto3.proto; \
	  $(PROTOC) --proto_path=$(dir $(CONFORMANCE_PROTO_DIR)) --cpp_out=$(CONF_GEN) conformance/conformance.proto; \
	  $(CXX) $(CONFORMANCE_CXXFLAGS) $(CONF_INC) -I$(CONF_GEN) $(CONF_PB_CFLAGS) -o $(BUILD)/conformance_testee \
	    conformance_testee.cc $(CONF_GEN)/google/protobuf/test_messages_proto3.pb.cc $(CONF_GEN)/conformance/conformance.pb.cc \
	    $(PPB_ROOT)/build/libppb.a $(CONF_PB_LIBS); \
	  if [ -x "$(CONFORMANCE_RUNNER)" ]; then \
	    LD_LIBRARY_PATH="$(PROTOBUF_LIBDIR)" $(CONFORMANCE_RUNNER) --enforce_recommended --failure_list failure_list_proto3.txt $(BUILD)/conformance_testee; \
	  else \
	    echo "conformance_testee built; set CONFORMANCE_RUNNER=<path> to run the suite"; \
	  fi; \
	else \
	  echo "conformance: protobuf prefix not built; run 'make -C differential protobuf-prefix' (or 'make conformance-docker'), skipping"; \
	fi

# proto2: same full sink; drops group/extension fields.
conformance-proto2: $(PPB_ROOT)/build/libppb.a
	@if $(have_conformance); then \
	  set -e; mkdir -p $(CONF_GEN_PROTO2); \
	  $(PROTOC) $(PROD_PLUGIN) --proto_path=$(PROTOBUF_SRC) --ppb_opt=$(CONF_OPT_PROTO2) --ppb_out=$(CONF_GEN_PROTO2) google/protobuf/test_messages_proto2.proto; \
	  $(PROTOC) $(REFLECT_PLUGIN) --proto_path=$(PROTOBUF_SRC) --ppb_opt=$(CONF_OPT_PROTO2) --ppb_out=$(CONF_GEN_PROTO2) google/protobuf/test_messages_proto2.proto; \
	  $(PROTOC) --proto_path=$(PROTOBUF_SRC) --cpp_out=$(CONF_GEN_PROTO2) google/protobuf/test_messages_proto2.proto; \
	  $(PROTOC) --proto_path=$(dir $(CONFORMANCE_PROTO_DIR)) --cpp_out=$(CONF_GEN_PROTO2) conformance/conformance.proto; \
	  $(CXX) -std=c++20 -O2 $(CONF_MCMODEL) -I$(CONF_GEN_PROTO2) $(CONF_PB_CFLAGS) -c $(CONF_GEN_PROTO2)/google/protobuf/test_messages_proto2.pb.cc -o $(CONF_GEN_PROTO2)/tm2.o; \
	  $(CXX) $(CONFORMANCE_CXXFLAGS) $(CONF_INC) -I$(CONF_GEN_PROTO2) $(CONF_PB_CFLAGS) -c conformance_testee_proto2.cc -o $(CONF_GEN_PROTO2)/testee.o; \
	  $(CXX) -std=c++20 -O2 $(CONF_MCMODEL) -I$(CONF_GEN_PROTO2) $(CONF_PB_CFLAGS) -c $(CONF_GEN_PROTO2)/conformance/conformance.pb.cc -o $(CONF_GEN_PROTO2)/conf.o; \
	  $(CXX) $(CONF_MCMODEL) -o $(BUILD)/conformance_testee_proto2 $(CONF_GEN_PROTO2)/testee.o $(CONF_GEN_PROTO2)/tm2.o $(CONF_GEN_PROTO2)/conf.o $(PPB_ROOT)/build/libppb.a $(CONF_PB_LIBS); \
	  if [ -x "$(CONFORMANCE_RUNNER)" ]; then \
	    LD_LIBRARY_PATH="$(PROTOBUF_LIBDIR)" $(CONFORMANCE_RUNNER) --failure_list failure_list_proto2.txt $(BUILD)/conformance_testee_proto2; \
	  else \
	    echo "conformance_testee_proto2 built; set CONFORMANCE_RUNNER=<path> to run the suite"; \
	  fi; \
	else \
	  echo "conformance-proto2: protobuf prefix not built; run 'make -C differential protobuf-prefix' (or 'make conformance-docker'), skipping"; \
	fi

# proto3 lean: both wire forms emitted, the non-canonical one marked
# field_semantics::error (rejected at parse); no unknown-field retention.
conformance-lean: $(PPB_ROOT)/build/libppb.a
	@if $(have_conformance); then \
	  set -e; mkdir -p $(CONF_GEN_LEAN); \
	  $(PROTOC) $(PROD_PLUGIN) --proto_path=$(PROTOBUF_SRC) --ppb_opt=$(CONF_OPT_LEAN) --ppb_out=$(CONF_GEN_LEAN) google/protobuf/test_messages_proto3.proto; \
	  $(PROTOC) $(REFLECT_PLUGIN) --proto_path=$(PROTOBUF_SRC) --ppb_opt=$(CONF_OPT_LEAN) --ppb_out=$(CONF_GEN_LEAN) google/protobuf/test_messages_proto3.proto; \
	  $(PROTOC) --proto_path=$(PROTOBUF_SRC) --cpp_out=$(CONF_GEN_LEAN) google/protobuf/test_messages_proto3.proto; \
	  $(PROTOC) --proto_path=$(dir $(CONFORMANCE_PROTO_DIR)) --cpp_out=$(CONF_GEN_LEAN) conformance/conformance.proto; \
	  $(CXX) $(CONFORMANCE_CXXFLAGS) $(CONF_INC) -I$(CONF_GEN_LEAN) $(CONF_PB_CFLAGS) -o $(BUILD)/conformance_testee_lean \
	    conformance_testee.cc $(CONF_GEN_LEAN)/google/protobuf/test_messages_proto3.pb.cc $(CONF_GEN_LEAN)/conformance/conformance.pb.cc \
	    $(PPB_ROOT)/build/libppb.a $(CONF_PB_LIBS); \
	  if [ -x "$(CONFORMANCE_RUNNER)" ]; then \
	    LD_LIBRARY_PATH="$(PROTOBUF_LIBDIR)" $(CONFORMANCE_RUNNER) --enforce_recommended --failure_list failure_list_proto3_lean.txt $(BUILD)/conformance_testee_lean; \
	  else \
	    echo "conformance_testee_lean built; set CONFORMANCE_RUNNER=<path> to run the suite"; \
	  fi; \
	else \
	  echo "conformance-lean: protobuf prefix not built; run 'make -C differential protobuf-prefix' (or 'make conformance-docker'), skipping"; \
	fi

# Deterministic subset run in CI (no Docker, no time-bounded fuzzing).
ci: test-utf8 reflection-diff reflection-random-diff fuzz-sink-replay structured-replay
	@echo "differential ci: deterministic subset OK"
