# differential

Everything that differentially tests PPB against libprotobuf lives here: a
second `protoc` plugin, `protoc_gen_ppb_reflect.py`, that emits a reflection
sink (`foo.ppb.reflect.hpp`) built on top of the production schema header from
`generator/protoc-gen-ppb`; the sink's `parse_into`/`merge_into` decode PPB
wire input into a libprotobuf `Message` via reflection, so the result can be
compared against libprotobuf's parser. This directory also holds the
differential drivers, the reflection-sink runtime unit tests, the sink
fuzzers, and the protobuf conformance testees. Production `generator/` stays
free of libprotobuf code; only this tree links against it.

## Dependencies

Targets look for their dependency and print a skip message instead of failing
when it's absent:

- apt `libprotobuf-dev` + `protobuf-compiler`: the reflection differentials,
  runtime unit tests, and byte-fuzzer replay.
- libprotobuf-mutator (rarely packaged; a sibling `../libprotobuf-mutator/_install`
  build or a system install, whichever is found; CI builds it from a known
  tag): `structured-replay`.
- a pinned protobuf checkout+prefix (sibling `../protobuf/_install`, built by
  `make -C differential protobuf-prefix`, or baked into the Docker image):
  the conformance suites.

## Entry points

- `make -C differential ci`: the deterministic subset, also runs in CI.
- `fuzz-sink` / `fuzz-structured`: time-bounded fuzzing; not run in CI.
- `conformance`, `conformance-proto2`, `conformance-lean`: the protobuf
  conformance suites.  The testees only support little-endian hosts.
- `conformance-docker`: the same three suites, self-contained in a
  container image.

Host runs and in-image runs share `build/`; remember to `make -C differential clean`.
