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

# variant <opt> <suffix> <proto>: emit <base>.<suffix>.ppb.hpp into the corpus.
variant() {
    local opt="$1" suffix="$2" proto="$3"
    local stem="${proto%.proto}"
    protoc --plugin=protoc-gen-ppb="$plugin" --proto_path=testdata \
           --ppb_opt="$opt" --ppb_out="$tmp" "$proto"
    mv "$tmp/$stem.ppb.hpp" "$out/$stem.$suffix.ppb.hpp"
}

# Default (mode=lean) schema headers.
base "" keywords3.proto
base "" nested2.proto
base "" nsclash3.proto
base "" stdshadow3.proto
base "" ppbshadow3.proto
base "opaque_cycles" stdshadow_rec3.proto
base "" empty3.proto
base "" maps3.proto
base "" enumref3.proto
base "" leanrep3.proto
# Both files in one invocation: exercises generate()'s multi-file loop and the
# global emission order spanning a file boundary.
base "" xfile_base3.proto xfile_main3.proto

# Recursive graphs need opaque_cycles to cut back-edges.
base "opaque_cycles" recursive3.proto
base "opaque_cycles" pbrecursive.proto
base "opaque_cycles" pbmrec.proto
base "opaque_cycles" pbrrec.proto

# Real oneof decoded as independent last_write_wins fields.
base "oneof_as_optional" oneof3.proto

# Repeated-scalar/enum protos: none diverges from lean on the alt wire form,
# full additionally detects unknowns.
variant "mode=none" "none" leanrep3.proto
variant "mode=full" "full" leanrep3.proto

# Message-bearing protos: full adds detect_unknown_fields<> to every message.
variant "mode=full" "full" nested2.proto
variant "mode=full" "full" maps3.proto
variant "mode=full" "full" empty3.proto
variant "mode=full,opaque_cycles" "full" recursive3.proto
variant "mode=full,opaque_cycles" "full" pbrecursive.proto
variant "mode=full,opaque_cycles" "full" pbmrec.proto
variant "mode=full,opaque_cycles" "full" pbrrec.proto
variant "mode=full,oneof_as_optional" "full" oneof3.proto

# Drop-flag variants.
# drop_foreign_type_fields: drops a field referencing an unsupported WKT (struct).
variant "drop_foreign_type_fields" "drop_imports" foreigndrop2.proto
# drop_group_extension_fields: drops a proto2 group field.
protoc --plugin=protoc-gen-ppb="$plugin" --proto_path=testdata/invalid \
       --ppb_opt=drop_group_extension_fields --ppb_out="$tmp" group.proto
mv "$tmp/group.ppb.hpp" "$out/groupdrop2.ppb.hpp"

# Proto2 required-warning: exercises required-downgrade and default-value warnings.
base "" reqwarn2.proto

echo "regenerated goldens in $out"
