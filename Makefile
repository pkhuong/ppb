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

.PHONY: all clean format unit unit_cpp test regen_test fuzz fuzz-corpus compile_fail analyze-clang analyze-gcc FORCE

all: build/libppb.a build/libppb.so build/picoscope build/ubench

include frama-c.mk

clean:
	rm -rf build/ wp.csv eva.csv

format:
	clang-format-20 -i include/ppb/ppb.h include/ppb/*.hpp src/*.[ch] examples/*.c tests/*.c tests/*.cc fuzz/*.c

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

build/fuzz_ppb: fuzz/fuzz_ppb.c src/ppb.c
	@mkdir -p $(dir $@)
	$(FUZZ_CC) $(FUZZ_CFLAGS) -o $@ fuzz/fuzz_ppb.c src/ppb.c

fuzz-corpus: fuzz/gen_corpus.py
	python3 fuzz/gen_corpus.py fuzz/corpus

fuzz: build/fuzz_ppb fuzz-corpus
	build/fuzz_ppb -max_total_time=$(FUZZ_TIME) -max_len=$(FUZZ_MAX_LEN) $(FUZZ_FLAGS) fuzz/corpus

analyze-clang: clean
	scan-build --status-bugs $(MAKE) CC=clang CXX=clang++ build/libppb.a build/test_ppb_cpp build/test_ppb_cpp_static

analyze-gcc: clean
	$(MAKE) CC=gcc CXX=g++ EXTRA_FLAGS="-fanalyzer" build/libppb.a build/test_ppb_cpp build/test_ppb_cpp_static

FORCE:
.SECONDARY:  # don't rm "temporary" (like our .o) output files at the end
