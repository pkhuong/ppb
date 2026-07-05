# Pinned build environment for the differential/conformance test subsystem.
# Everything the suites need is baked in: no repo other than PPB itself has to
# exist on the host.  The PPB repo is bind-mounted at run time (at /src for the
# fuzzers, /work/ppb for conformance; see fuzz/build_and_run.sh and the
# `conformance-docker` Makefile target); it is not baked in.  Two pinned
# from-source builds justify containerizing:
#
#   * libprotobuf-mutator (LPM), not packaged in Debian trixie, built from a
#     pinned tag against the distro libprotobuf, for the structure-aware fuzzers.
#   * protobuf itself, at the pinned release the committed conformance failure
#     lists track, installed to /work/protobuf (a sibling of the /work/ppb mount)
#     with conformance_test_runner and the test_messages/conformance .protos, so
#     the same `make conformance*` runs in-image and on the host, no host
#     protobuf checkout required.

FROM docker.io/library/debian:trixie-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        clang \
        libclang-rt-dev \
        g++ \
        cmake \
        git \
        make \
        pkg-config \
        protobuf-compiler \
        libprotobuf-dev \
        libjsoncpp-dev \
        zlib1g-dev \
        python3 \
        python3-pip \
    && rm -rf /var/lib/apt/lists/*

# Build libprotobuf-mutator from the pinned tag, against the distro
# libprotobuf (DOWNLOAD_PROTOBUF=OFF).  v1.5 =
# dc4ced337a9fb4047e2dc727268fbac55ca82f73.
ARG LPM_TAG=v1.5
RUN git clone --depth 1 --branch "$LPM_TAG" \
        https://github.com/google/libprotobuf-mutator /opt/lpm \
    && cmake -S /opt/lpm -B /opt/lpm/build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang \
        -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_CXX_STANDARD=17 \
        -DLIB_PROTO_MUTATOR_DOWNLOAD_PROTOBUF=OFF \
        -DLIB_PROTO_MUTATOR_TESTING=OFF \
        -DLIB_PROTO_MUTATOR_EXAMPLES=OFF \
    && cmake --build /opt/lpm/build -j"$(nproc)" \
    && cmake --install /opt/lpm/build \
    && rm -rf /opt/lpm

# Build protobuf + conformance_test_runner from the pinned release the committed
# failure lists track, and lay it out as a SIBLING of where the checkout mounts
# (/work/protobuf next to the /work/ppb bind mount, see the conformance-docker
# target).  That way the differential Makefile's default PROTOBUF_DIR
# ($(PPB_ROOT)/../protobuf) resolves to it and the very same `make conformance*`
# runs in-image and on the host with no overrides.  setup_protobuf.sh is the one
# source of truth for this build, shared with `make -C differential
# protobuf-prefix`; it installs to /work/protobuf/_install and keeps the source
# tree for the test_messages/conformance .protos.  CMake auto-fetches abseil +
# utf8_range (pinned by protobuf's dependencies.cmake); jsoncpp for the runner
# comes from the distro (libjsoncpp-dev).  The prefix is separate from the apt
# libprotobuf the LPM fuzzers link, so those stay untouched.
ARG PROTOBUF_REF=v35.0
COPY differential/setup_protobuf.sh /tmp/setup_protobuf.sh
RUN PROTOBUF_REF="$PROTOBUF_REF" sh /tmp/setup_protobuf.sh /work/protobuf /work/protobuf/_install \
    && rm -rf /work/protobuf/.git

# The protoc plugins run via uv, matching the host `#!/usr/bin/env -S uv run
# --script` shebang: codegen gets a current protobuf despite the stale distro
# python3-protobuf, and no .venv lands in the bind-mounted repo.  Use the apt
# python3 (UV_PYTHON_PREFERENCE=only-system: no managed-Python download).
ENV UV_PYTHON_PREFERENCE=only-system
RUN pip3 install --break-system-packages --no-cache-dir --root-user-action=ignore uv

# Prime uv's env cache at build time so `docker run` needs no network.  uv keys
# the env by resolved deps + interpreter, so running each plugin warms the
# very env it reuses at run time; the plugin's `# /// script` block is the only
# place the deps live.  reflect.py imports protoc_gen_ppb from ../generator, so
# copy the two as siblings; empty stdin makes a plugin generate nothing and exit
# 0, after uv has cached the env.  The copies are transient; at run time the
# bind mount at /src supplies the actual scripts.
COPY generator/protoc_gen_ppb.py /tmp/warm/generator/protoc_gen_ppb.py
COPY differential/protoc_gen_ppb_reflect.py /tmp/warm/differential/protoc_gen_ppb_reflect.py
RUN uv run --script /tmp/warm/generator/protoc_gen_ppb.py </dev/null >/dev/null \
    && uv run --script /tmp/warm/differential/protoc_gen_ppb_reflect.py </dev/null >/dev/null \
    && rm -rf /tmp/warm

# Everything the plugins need is now in the uv cache; force offline at run time
# so `uv run --script` never revalidates `protobuf>=4` against the index (which
# would fail with no network, and drift with one).  The warm step above ran
# before this, while the network was available.
ENV UV_OFFLINE=1

WORKDIR /src
CMD ["sh", "differential/fuzz/build_and_run.sh"]
