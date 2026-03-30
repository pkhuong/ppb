CC := ccache gcc
COMMON_FLAGS := -O2 -Iinclude/ -Wall -Wextra -Wpedantic -ggdb
CFLAGS := -std=c2x $(COMMON_FLAGS)

C_SRCS := src/ppb.c

STATIC_DIR := build/static
SHARED_DIR := build/shared

STATIC_C_OBJS := $(patsubst src/%.c,$(STATIC_DIR)/%.o,$(C_SRCS))
STATIC_OBJS := $(STATIC_C_OBJS)

SHARED_C_OBJS := $(patsubst src/%.c,$(SHARED_DIR)/%.o,$(C_SRCS))
SHARED_OBJS := $(SHARED_C_OBJS)

.PHONY: all clean unit FORCE

all: build/libppb.a build/libppb.so build/picoscope

include frama-c.mk

unit: build/test_ppb
	build/test_ppb

clean:
	rm -rf build/ wp.csv eva.csv

build/libppb.a: $(patsubst %,%.hash,$(STATIC_OBJS)) | $(STATIC_OBJS)
	@rm -f $@
	ar rcs $@ $(STATIC_OBJS)

build/libppb.so: $(patsubst %,%.hash,$(SHARED_OBJS)) | $(SHARED_OBJS)
	$(CC) -shared -Wl,-soname,libppb.so -Wl,--no-undefined -o $@ $(SHARED_OBJS)

build/test_ppb: tests/test_ppb.c src/ppb.c include/ppb/ppb.h $(wildcard src/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I. -o $@ tests/test_ppb.c src/ppb.c

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

FORCE:
.SECONDARY:  # don't rm "temporary" (like our .o) output files at the end
