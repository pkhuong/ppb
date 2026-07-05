#!/bin/sh
# Build protobuf + conformance_test_runner from a pinned upstream commit and
# install them (plus the test_messages/conformance .protos, kept in the source
# tree) under a prefix, for the differential conformance suite.
#
# Shared by the differential Dockerfile (baked into the image) and the local
# `make -C differential protobuf-prefix` target, so the in-container and on-host
# toolchains are byte-identical; the same `make conformance*` then runs in
# both.  Idempotent: a no-op once the runner is installed.
#
# CMake auto-fetches abseil and utf8_range (pinned by protobuf's own
# dependencies.cmake, build-time network); jsoncpp for the runner is expected
# from the system (Debian libjsoncpp-dev) so there is no runtime .so to chase.
#
# Usage: setup_protobuf.sh <checkout-dir> <install-prefix>
set -eu

SRC="${1:?usage: setup_protobuf.sh <checkout-dir> <install-prefix>}"
PREFIX="${2:?usage: setup_protobuf.sh <checkout-dir> <install-prefix>}"

# The pinned protobuf release the committed failure lists track.
REF="${PROTOBUF_REF:-v35.0}"

# A pre-existing checkout must sit exactly on the pinned ref; a wrong-ref tree
# (e.g. a v35-dev checkout) silently alters the conformance corpus and surfaces
# as bogus "unexpected" failures.  Refuse to reuse it.
if [ -e "$SRC/.git" ] && \
   [ "$(git -C "$SRC" rev-parse HEAD)" != "$(git -C "$SRC" rev-parse "refs/tags/$REF^{commit}" 2>/dev/null)" ]; then
    echo "setup_protobuf: $SRC is not at pinned $REF; remove it (or point PROTOBUF_DIR elsewhere) and re-run" >&2
    exit 1
fi

if [ -x "$PREFIX/bin/conformance_test_runner" ]; then
    echo "setup_protobuf: $PREFIX/bin/conformance_test_runner already present, skipping"
    exit 0
fi

if [ ! -e "$SRC/src/google/protobuf/test_messages_proto3.proto" ]; then
    git clone --depth 1 --branch "$REF" \
        https://github.com/protocolbuffers/protobuf.git "$SRC"
fi

cmake -S "$SRC" -B "$SRC/_build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -Dprotobuf_BUILD_TESTS=OFF \
    -Dprotobuf_BUILD_CONFORMANCE=ON \
    -Dprotobuf_BUILD_SHARED_LIBS=ON \
    -Dprotobuf_INSTALL=ON
cmake --build "$SRC/_build" -j"$(nproc)"
cmake --install "$SRC/_build"

# conformance_test_runner is a build artifact, not installed by `cmake --install`.
cp "$(find "$SRC/_build" -name conformance_test_runner -type f | head -1)" \
    "$PREFIX/bin/conformance_test_runner"
rm -rf "$SRC/_build"

echo "setup_protobuf: installed protobuf + conformance_test_runner to $PREFIX"
