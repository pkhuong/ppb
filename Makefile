CC := ccache gcc
EXTRA_FLAGS :=
COMMON_FLAGS := -O2 -Iinclude/ -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -ggdb $(EXTRA_FLAGS)
CFLAGS := -std=c11 $(COMMON_FLAGS)

CXX := ccache g++
CXXFLAGS := -std=c++20 $(COMMON_FLAGS)

C_SRCS := src/ppb.c
CPP_HEADERS := include/ppb/ppb.hpp include/ppb/ppb_detail.hpp

STATIC_DIR := build/static
SHARED_DIR := build/shared

STATIC_C_OBJS := $(patsubst src/%.c,$(STATIC_DIR)/%.o,$(C_SRCS))
STATIC_OBJS := $(STATIC_C_OBJS)

SHARED_C_OBJS := $(patsubst src/%.c,$(SHARED_DIR)/%.o,$(C_SRCS))
SHARED_OBJS := $(SHARED_C_OBJS)

PROTOSCOPE ?= protoscope

.PHONY: all clean format unit unit_cpp test regen_test fuzz fuzz-corpus compile_fail analyze-clang analyze-gcc tysan fuzz_cpp fuzz_cpp_msan fuzz_cpp_tysan sweep sweep_cpp sweep-seeds generator_test FORCE

all: build/libppb.a build/libppb.so build/picoscope build/ubench

include frama-c.mk

clean:
	rm -rf build/ wp.csv eva.csv

format:
	clang-format-20 -i include/ppb/ppb.h include/ppb/*.hpp src/*.[ch] examples/*.c tests/*.c tests/*.cc fuzz/*.c fuzz/*.cc generator/testdata/*.cc differential/*.cc differential/*.hpp differential/fuzz/*.cc differential/fuzz/*.hpp

unit: build/test_ppb
	build/test_ppb

unit_cpp: build/test_ppb_cpp build/test_ppb_cpp_static compile_fail
	build/test_ppb_cpp
	build/test_ppb_cpp_static

test: build/picoscope build/test_ppb
	build/test_ppb
	sh test_picoscope.sh build/picoscope

regen_test: build/picoscope
	sh test_picoscope.sh -g $(PROTOSCOPE) build/picoscope

build/libppb.a: $(patsubst %,%.hash,$(STATIC_OBJS)) | $(STATIC_OBJS)
	@rm -f $@
	ar rcs $@ $(STATIC_OBJS)

build/libppb.so: $(patsubst %,%.hash,$(SHARED_OBJS)) | $(SHARED_OBJS)
	$(CC) $(COMMON_FLAGS) -shared -Wl,-soname,libppb.so -Wl,--no-undefined -o $@ $(SHARED_OBJS)

build/picoscope: examples/picoscope.c build/libppb.a FORCE
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $< build/libppb.a

build/ubench: examples/ubench.c build/libppb.a FORCE
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $< build/libppb.a -lm

build/test_ppb: tests/test_ppb.c FORCE
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I. -o $@ tests/test_ppb.c src/ppb.c

build/test_ppb_cpp: tests/test_ppb_cpp.cc $(CPP_HEADERS) build/libppb.a FORCE
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -I. -o $@ tests/test_ppb_cpp.cc build/libppb.a

build/test_ppb_cpp_static: tests/test_ppb_cpp_static.cc $(CPP_HEADERS) build/libppb.a FORCE
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -I. -o $@ tests/test_ppb_cpp_static.cc build/libppb.a

compile_fail: $(CPP_HEADERS) tests/test_ppb_cpp_compile_fail.cc tests/compile_fail.py
	python3 tests/compile_fail.py "$(CXX)" "$(CXXFLAGS) -I." tests/test_ppb_cpp_compile_fail.cc

$(STATIC_DIR)/%.o: src/%.c FORCE
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(SHARED_DIR)/%.o: src/%.c FORCE
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fPIC -c -o $@ $<

# Simplified version of https://www.kolpackov.net/pipermail/notes/2004-September/000011.html
#
# ccache is nicer than mtime-based invalidation, but we don't have "ldcache", so just try to
# avoid relinking when there's nothing to do.

build/%.hash: build/%
	@h=$$(sha256sum $<) || exit 1; echo "$$h" | cmp -s $@ - || echo "$$h" > $@

FUZZ_CC ?= clang
FUZZ_CFLAGS := $(CFLAGS) -g -fsanitize=fuzzer,address,undefined

# Knobs for `make fuzz`.  Override on the command line, e.g.
#   make fuzz FUZZ_TIME=3600 FUZZ_MAX_LEN=4096 FUZZ_FLAGS='-jobs=8 -workers=8'
FUZZ_TIME ?= 60
FUZZ_MAX_LEN ?= 1024
FUZZ_FLAGS ?=

build/fuzz_ppb: fuzz/fuzz_ppb.c src/ppb.c FORCE
	@mkdir -p $(dir $@)
	$(FUZZ_CC) $(FUZZ_CFLAGS) -o $@ fuzz/fuzz_ppb.c src/ppb.c

fuzz-corpus: fuzz/gen_corpus.py
	python3 fuzz/gen_corpus.py fuzz/corpus

fuzz: build/fuzz_ppb fuzz-corpus
	build/fuzz_ppb -max_total_time=$(FUZZ_TIME) -max_len=$(FUZZ_MAX_LEN) $(FUZZ_FLAGS) fuzz/corpus

FUZZ_CXX ?= clang++
FUZZ_CXXFLAGS := $(CXXFLAGS) -g -fsanitize=fuzzer,address,undefined

# Flags for compiling the C core as an instrumented object linked into
# the C++ fuzzers binary (i.e., without libFuzzer main)
FUZZ_C_ASAN_FLAGS := $(CFLAGS) -g -fsanitize=fuzzer-no-link,address,undefined

build/fuzz/ppb_asan.o: src/ppb.c FORCE
	@mkdir -p $(dir $@)
	$(FUZZ_CC) $(FUZZ_C_ASAN_FLAGS) -c -o $@ $<

build/fuzz_ppb_cpp: fuzz/fuzz_ppb_cpp.cc build/fuzz/ppb_asan.o $(CPP_HEADERS)
	@mkdir -p $(dir $@)
	$(FUZZ_CXX) $(FUZZ_CXXFLAGS) -o $@ fuzz/fuzz_ppb_cpp.cc build/fuzz/ppb_asan.o

fuzz_cpp: build/fuzz_ppb_cpp fuzz-corpus
	build/fuzz_ppb_cpp -max_total_time=$(FUZZ_TIME) -max_len=$(FUZZ_MAX_LEN) $(FUZZ_FLAGS) fuzz/corpus

# Feed a deterministic enumeration of mutated inputs to preexisting fuzzers.
FUZZ_CXXFLAGS_NOLINK := $(CXXFLAGS) -g -fsanitize=fuzzer-no-link,address,undefined

sweep-seeds: fuzz/gen_corpus.py
	@mkdir -p build/sweep_seeds
	python3 fuzz/gen_corpus.py build/sweep_seeds

build/sweep_ppb_cpp: fuzz/sweep_main.cc fuzz/fuzz_ppb_cpp.cc build/fuzz/ppb_asan.o $(CPP_HEADERS)
	@mkdir -p $(dir $@)
	$(FUZZ_CXX) $(FUZZ_CXXFLAGS_NOLINK) -o $@ fuzz/sweep_main.cc fuzz/fuzz_ppb_cpp.cc build/fuzz/ppb_asan.o

sweep_cpp: build/sweep_ppb_cpp sweep-seeds
	build/sweep_ppb_cpp build/sweep_seeds/*

sweep: sweep_cpp

# MemorySanitizer fuzzing build.  MSan needs the C core instrumented,
# like ASan.
FUZZ_MSAN_CXXFLAGS := $(CXXFLAGS) -g -fsanitize=fuzzer,memory -fsanitize-memory-track-origins
FUZZ_C_MSAN_FLAGS := $(CFLAGS) -g -fsanitize=fuzzer-no-link,memory -fsanitize-memory-track-origins

build/fuzz/ppb_msan.o: src/ppb.c FORCE
	@mkdir -p $(dir $@)
	$(FUZZ_CC) $(FUZZ_C_MSAN_FLAGS) -c -o $@ $<

build/fuzz_ppb_cpp_msan: fuzz/fuzz_ppb_cpp.cc build/fuzz/ppb_msan.o $(CPP_HEADERS)
	@mkdir -p $(dir $@)
	$(FUZZ_CXX) $(FUZZ_MSAN_CXXFLAGS) -o $@ fuzz/fuzz_ppb_cpp.cc build/fuzz/ppb_msan.o

fuzz_cpp_msan: build/fuzz_ppb_cpp_msan fuzz-corpus
	build/fuzz_ppb_cpp_msan -max_total_time=$(FUZZ_TIME) -max_len=$(FUZZ_MAX_LEN) $(FUZZ_FLAGS) fuzz/corpus

# TypeSanitizer fuzzing build: instrument the C++ TU only and link the
# uninstrumented libppb.a (instrumenting the C core introduces a bunch
# of cross-language false positives).  The primary intended target is
# the type punning around fixed-width packed types.
FUZZ_TYSAN_CXXFLAGS := $(CXXFLAGS) -O1 -g -fsanitize=fuzzer,type

build/fuzz_ppb_cpp_tysan: fuzz/fuzz_ppb_cpp.cc $(CPP_HEADERS) build/libppb.a
	@mkdir -p $(dir $@)
	$(FUZZ_CXX) $(FUZZ_TYSAN_CXXFLAGS) -o $@ fuzz/fuzz_ppb_cpp.cc build/libppb.a

# TySan reports and continues; treat any type-aliasing-violation (or a
# non-zero exit) as failure.  Hardcoded -runs because there isn't much
# to explore.
fuzz_cpp_tysan: build/fuzz_ppb_cpp_tysan fuzz-corpus
	@out=$$(build/fuzz_ppb_cpp_tysan -runs=1000000 -max_len=$(FUZZ_MAX_LEN) $(FUZZ_FLAGS) fuzz/corpus 2>&1); rc=$$?; \
	printf '%s\n' "$$out"; \
	if [ $$rc -ne 0 ] || printf '%s' "$$out" | grep -q 'type-aliasing-violation'; then \
	    echo "FAIL: TySan violation or non-zero exit"; exit 1; \
	fi

analyze-clang: clean
	scan-build --status-bugs $(MAKE) CC=clang CXX=clang++ build/libppb.a build/test_ppb_cpp build/test_ppb_cpp_static

analyze-gcc: clean
	$(MAKE) CC=gcc CXX=g++ EXTRA_FLAGS="-fanalyzer" build/libppb.a build/test_ppb_cpp build/test_ppb_cpp_static

# Clang TypeSanitizer (TBAA / strict-aliasing) build for the C++ wrapper.
# Looks for type issues in the C++ type safety fanciness.
#
# We instrument the C++ TUs *only* and link them against the normal,
# uninstrumented libppb.a.  Instrumenting the C core as well only
# surfaces a ton of cross-language false positives.
TYSAN_CXX ?= clang++
TYSAN_CXXFLAGS := $(CXXFLAGS) -O1 -fsanitize=type

build/tysan/test_ppb_cpp: tests/test_ppb_cpp.cc $(CPP_HEADERS) build/libppb.a FORCE
	@mkdir -p $(dir $@)
	$(TYSAN_CXX) $(TYSAN_CXXFLAGS) -I. -o $@ tests/test_ppb_cpp.cc build/libppb.a

build/tysan/test_ppb_cpp_static: tests/test_ppb_cpp_static.cc $(CPP_HEADERS) build/libppb.a FORCE
	@mkdir -p $(dir $@)
	$(TYSAN_CXX) $(TYSAN_CXXFLAGS) -I. -o $@ tests/test_ppb_cpp_static.cc build/libppb.a

# TySan reports each violation and continues; have to look for reports
# and exit non-zero here.
tysan: build/tysan/test_ppb_cpp build/tysan/test_ppb_cpp_static
	@status=0; \
	for t in $^; do \
	    echo "TYSAN $$t"; \
	    out=$$("$$t" 2>&1); rc=$$?; \
	    printf '%s\n' "$$out"; \
	    if [ $$rc -ne 0 ] || printf '%s' "$$out" | grep -q 'type-aliasing-violation'; then \
	        echo "FAIL: TySan violation or non-zero exit in $$t"; status=1; \
	    fi; \
	done; \
	exit $$status

generator_test:
	@command -v protoc >/dev/null 2>&1 || { echo "generator_test: protoc not found, skipping"; exit 0; }
	@command -v uv >/dev/null 2>&1 || { echo "generator_test: uv not found, skipping"; exit 0; }
	$(MAKE) -C generator check
	cd generator && ./regen_golden.sh
	git diff --exit-code generator/testdata/golden || { echo "golden headers out of date; run generator/regen_golden.sh and commit"; exit 1; }
	cd generator && ./regen_invalid.py
	git diff --exit-code generator/testdata/invalid || { echo "invalid-input goldens out of date; run generator/regen_invalid.py and commit"; exit 1; }
	mkdir -p build
	cd generator && uv run python3 protoc_gen_ppb.py --emit-wkt-bundle ../build/ppb_wkt_check.hpp
	diff -u include/ppb/wkt.ppb.hpp build/ppb_wkt_check.hpp || { echo "include/ppb/wkt.ppb.hpp out of date"; exit 1; }
	for h in generator/testdata/golden/*.ppb.hpp; do \
		$(CXX) $(CXXFLAGS) -fsyntax-only -Wno-pragma-once-outside-header "$$h" || exit 1; \
	done
	$(CXX) $(CXXFLAGS) -o build/ppb_gen_compile generator/testdata/test_compile.cc
	$(CXX) $(CXXFLAGS) -fsyntax-only generator/testdata/test_compile_variants.cc
	$(MAKE) build/libppb.a
	$(CXX) $(CXXFLAGS) -Igenerator/testdata -o build/test_wkt_runtime generator/tests/test_wkt_runtime.cc build/libppb.a
	./build/test_wkt_runtime && echo "test_wkt_runtime OK"
	@echo "generator_test OK"

FORCE:
.SECONDARY:  # don't rm "temporary" (like our .o) output files at the end
