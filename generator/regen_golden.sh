#!/usr/bin/env bash
# generator/regen_golden.sh: regenerate committed golden headers.

set -euo pipefail
cd "$(dirname "$0")"
out="testdata/golden"
plugin="$PWD/protoc_gen_ppb.py"
mkdir -p "$out"
rm -f "$out"/*.ppb.hpp

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# base <opt> <proto...>: emit <base>.ppb.hpp into the corpus.
base() {
    local opt="$1"
    shift
    if [ -n "$opt" ]; then
        protoc --plugin=protoc-gen-ppb="$plugin" --proto_path=testdata \
               --ppb_opt="$opt" --ppb_out="$out" "$@"
    else
        protoc --plugin=protoc-gen-ppb="$plugin" --proto_path=testdata \
               --ppb_out="$out" "$@"
    fi
}

base "" ppbshadow3.proto
base "" enumref3.proto

echo "regenerated goldens in $out"
