#!/bin/sh
# Build and run the structure-aware reflection-sink fuzzers.  Runs both inside
# the differential/Dockerfile image (repo bind-mounted at /src) and natively
# from a checkout with libprotobuf-mutator installed; the repo root is resolved
# from this script's own location, so the caller's cwd does not matter.
#
#   MAX_TOTAL_TIME   seconds of fuzzing per target binary (default 60)
#   REPLAY=1         deterministic mode: replay a regenerated seed corpus once
#                    (-runs=0) instead of time-bounded fuzzing.  Used by CI.
#
# In fuzzing mode the corpora persist under differential/fuzz/structured_corpus/
# on the mount, so successive runs resume where the last one stopped.  In REPLAY
# mode the seeds are regenerated from fuzz/gen_corpus.py (no committed corpus).
set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
cd "$REPO_ROOT"

OUT=differential/build/structured
GEN="$OUT/gen"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
MAX_TOTAL_TIME="${MAX_TOTAL_TIME:-60}"
REPLAY="${REPLAY:-0}"

# libprotobuf-mutator headers/libs.  Default to the system install; the caller
# (make structured-replay) overrides these to point at a sibling LPM build.
LPM_INCLUDE="${LPM_INCLUDE:-/usr/local/include/libprotobuf-mutator}"
LPM_LIBDIR="${LPM_LIBDIR:-}"
LPM_LDFLAGS=""
if [ -n "$LPM_LIBDIR" ]; then
    LPM_LDFLAGS="-L$LPM_LIBDIR -Wl,-rpath,$LPM_LIBDIR"
fi

mkdir -p "$OUT" "$GEN"

# Run the plugins through uv, exactly like the host's `#!/usr/bin/env -S uv run
# --script` shebang: uv resolves each plugin's PEP 723 inline deps (protobuf) in
# a cached ephemeral env, so codegen ignores the stale distro python3-protobuf
# and writes no .venv into the mount.  Both plugins bind under the `ppb` key so
# --ppb_opt/--ppb_out resolve; they are separate protoc calls, one plugin per call.
printf '#!/bin/sh\nexec uv run --script %s/generator/protoc_gen_ppb.py "$@"\n' "$REPO_ROOT" \
    > "$OUT/protoc-gen-ppb-prod"
printf '#!/bin/sh\nexec uv run --script %s/differential/protoc_gen_ppb_reflect.py "$@"\n' "$REPO_ROOT" \
    > "$OUT/protoc-gen-ppb-reflect"
chmod +x "$OUT/protoc-gen-ppb-prod" "$OUT/protoc-gen-ppb-reflect"

protoc --proto_path=generator/testdata --cpp_out="$GEN" \
    pbscalars3.proto pbcomposite3.proto

# Full two-plugin recipe (matches the byte-fuzzer): production
# mode=full,always_dispatch_strings routes every proto3 string through the
# UTF-8-validating sink setter and bakes the unknown-field catch-all; reflect
# full retains unknown fields via the libprotobuf UnknownFieldSet so the sink
# stays at parity with libprotobuf's own re-parse.
protoc --plugin=protoc-gen-ppb="$OUT/protoc-gen-ppb-prod" \
    --proto_path=generator/testdata --ppb_opt=mode=full,always_dispatch_strings \
    --ppb_out="$GEN" pbscalars3.proto pbcomposite3.proto
protoc --plugin=protoc-gen-ppb="$OUT/protoc-gen-ppb-reflect" \
    --proto_path=generator/testdata --ppb_opt=mode=full,always_dispatch_strings \
    --ppb_out="$GEN" pbscalars3.proto pbcomposite3.proto

# Wrapper proto for the structured+byte corruption harness.
protoc --proto_path=differential/fuzz --proto_path=generator/testdata \
    --cpp_out="$GEN" differential/fuzz/fuzz_mutation.proto

clang -std=c11 -O1 -g $SAN -Iinclude -c src/ppb.c -o "$OUT/ppb.o"

for target in SCALARS MAPS REPEATED; do
    lower=$(echo "$target" | tr 'A-Z' 'a-z')
    clang++ -std=c++20 -O1 -g $SAN -fsanitize=fuzzer \
        "-DPPB_STRUCT_TARGET_$target" \
        -Iinclude -Idifferential -I"$GEN" -I"$LPM_INCLUDE" \
        $(pkg-config --cflags protobuf) \
        differential/fuzz/fuzz_sink_structured.cc \
        "$GEN/pbscalars3.pb.cc" "$GEN/pbcomposite3.pb.cc" "$OUT/ppb.o" \
        $LPM_LDFLAGS -lprotobuf-mutator-libfuzzer -lprotobuf-mutator \
        $(pkg-config --libs protobuf) \
        -o "$OUT/fuzz_sink_structured_$lower"
done

# Structured+byte corruption harness: full target x variant matrix.
for target in SCALARS MAPS REPEATED; do
    tl=$(echo "$target" | tr 'A-Z' 'a-z')
    for variant in ANY NO_OVERLONG ONE_TAG; do
        vl=$(echo "$variant" | tr 'A-Z' 'a-z')
        clang++ -std=c++20 -O1 -g $SAN -fsanitize=fuzzer \
            "-DPPB_MUT_TARGET_$target" "-DPPB_MUT_VARIANT_$variant" \
            -Iinclude -Idifferential -Idifferential/fuzz -I"$GEN" \
            -I"$LPM_INCLUDE" \
            $(pkg-config --cflags protobuf) \
            differential/fuzz/fuzz_sink_mutate.cc \
            "$GEN/fuzz_mutation.pb.cc" "$GEN/pbscalars3.pb.cc" "$GEN/pbcomposite3.pb.cc" "$OUT/ppb.o" \
            $LPM_LDFLAGS -lprotobuf-mutator-libfuzzer -lprotobuf-mutator \
            $(pkg-config --libs protobuf) \
            -o "$OUT/fuzz_sink_mutate_${tl}_${vl}"
    done
done

# Binary names have no spaces, so plain word-splitting over $bins is safe.
bins="fuzz_sink_structured_scalars fuzz_sink_structured_maps fuzz_sink_structured_repeated"
for tl in scalars maps repeated; do
    for vl in any no_overlong one_tag; do
        bins="$bins fuzz_sink_mutate_${tl}_${vl}"
    done
done

status=0

if [ "$REPLAY" = "1" ]; then
    # Deterministic: regenerate the shared seed corpus once and replay every
    # binary over it with -runs=0 (execute each seed once, then exit).  These
    # are LPM proto-fuzzers, so generic seeds mostly load as partial/empty
    # wrapper messages; the replay is a smoke test proving the binaries link,
    # LPM initializes, and the oracle survives the trivial-input paths without
    # a sanitizer trip.
    corpus="$OUT/replay_corpus"
    rm -rf "$corpus"
    python3 fuzz/gen_corpus.py "$corpus" >/dev/null

    for bin in $bins; do
        echo "=== $bin replay ==="

        if ! "$OUT/$bin" -runs=0 "$corpus"; then
            status=1
        fi
    done

    exit "$status"
fi

# Time-bounded fuzzing (local / Docker), one persistent corpus per binary.
for bin in $bins; do
    corpus="differential/fuzz/structured_corpus/$bin"
    mkdir -p "$corpus"
    echo "=== $bin (${MAX_TOTAL_TIME}s) ==="

    if ! "$OUT/$bin" -max_total_time="$MAX_TOTAL_TIME" -print_final_stats=1 "$corpus"; then
        status=1
    fi
done

exit "$status"
