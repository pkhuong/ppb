# PPB conformance gaps

This file documents every protobuf feature the PPB conformance testee does **not**
cover, and why. PPB is an allocation-free, non-recursive *lexer* for the protobuf
binary wire format, not a full implementation, so some conformance behavior is out
of scope by design, and some is deferred. Each gap below corresponds to conformance
tests that appear in one or more of the failure lists.

The testee decodes payloads with `protoc-gen-ppb`-generated `ppb::reader`s, writes
decoded fields into libprotobuf messages via reflection (the "reflection sink"),
and re-serializes with `Message::SerializeToString`.

## Scope

Three message types and three `make` targets:

- **`TestAllTypesProto3` (full)**: `make conformance`. The testee uses a
  `full`-mode schema (both packed and unpacked tags for repeated scalars) and an
  always-on `ppb::on_unknown` handler that writes unknown fields to the libprotobuf
  `UnknownFieldSet`. Failures are listed in `failure_list_proto3.txt`.

- **`TestAllTypesProto2` + `TestAllRequiredTypesProto2` (proto2)**: `make
  conformance-proto2`. Uses the same full schema, the always-on unknown-field
  retention handler, and `--ppb_opt=drop_group_extension_fields` to drop proto2 group
  and extension fields. The testee rejects messages missing required fields via
  libprotobuf's `IsInitialized()`. Failures are listed in `failure_list_proto2.txt`.

- **`TestAllTypesProto3` (lean)**: `make conformance-lean`. The lean schema emits
  both wire forms but marks the non-canonical one `field_semantics::error`, so a
  repeated field in its off-canonical encoding is rejected at `parse()`; it also has
  no unknown-field retention. Failures are listed in `failure_list_proto3_lean.txt`.

The testees only support protobuf output: JSON and text-format requests are always skipped.

## Gaps

### Unsupported WKT-typed fields (dropped)

Six trivial well-known types now decode: `Any`, `Duration`, `Empty`, `FieldMask`,
`Timestamp`, and the `wrappers.proto` `*Value` types. Their schemas are included
in `<ppb/wkt.ppb.hpp>`, so a field referencing one resolves like an ordinary
cross-file message reference and round-trips through the testee. Through the
reflection sink these six also decode through libprotobuf reflection, like any
other nested message.

In `full` mode the reflection sink also retains unknown fields
encountered inside a WKT scope.  The proto3 conformance suite passes
against the full testee with no expected failures.

Fields whose type is one of the *remaining* well-known types are still dropped from
the generated schema via `--ppb_opt=drop_foreign_type_fields`, and the testee never
decodes them: `Struct`, `Value`, `ListValue`, `NullValue`, `Type`, `Api`,
`SourceContext`, and the `descriptor.proto` messages. See `ppb-dropped:` comments at
the top of the generated `test_messages_proto3.ppb.hpp` header.

`Struct`/`Value`/`ListValue` are mutually recursive and would need
`opaque_cycles`; structural decode for the rest is deferred. A dropped field is
never structurally decoded, but its bytes still round-trip in the full and proto2
testees through the unknown-field retention handler. Lean mode has no retention
and would lose such a payload on re-serialization; no current binary conformance
test exercises these fields, so none of the failure lists has a WKT entry.

### oneof exclusivity (schema-level only, roundtrips fine)

User-declared oneofs are decoded via `--ppb_opt=oneof_as_optional`: each member becomes an
independent field. The PPB schema therefore does not *model* the oneof, but the
round-trip is still correct: the handlers write through libprotobuf reflection,
which enforces oneof exclusivity (setting any member clears the rest), and
last-occurrence across members matches the wire semantics.

### proto3 string UTF-8 validation (sink-level only)

Neither PPB nor the generated schemas validate UTF-8 (see the quirk lists in
the toplevel README and README_CPP.md). The reflection sink guards its string
setters with its own validator (`ppb_utf8.hpp`), rejecting invalid proto3
`string` payloads with `PPB_ERROR_INVALID_UTF8`, so the proto3 conformance
suite's required invalid-UTF-8 rejections pass because of sink-level code. A
plain `ppb::reader` user must validate in their handlers; see
`always_dispatch_strings` in generator/README.md for the dispatch-semantics
side of that.

### proto2 closed enums (sink-level only, corpus-untested)

The generated descriptors dispatch raw wire values for every enum, closed
proto2 enums included (see "Proto2 semantics" in generator/README.md). The
sink writes enum fields through libprotobuf reflection
(`SetEnumValue`/`AddEnumValue`), which itself routes out-of-range values on
closed enums to the unknown-field set, so the testee matches spec behavior
that a plain `ppb::reader` user does not get. Note that the binary
conformance corpus only exercises in-range enum values, and the randomized
differential deliberately draws enum values from the declared set, so neither
suite would surface the divergence either way.

## Residual divergences

All three suites pass with 0 unexpected failures:

| Suite                                 | Successes | Skipped | Expected failures |
|---------------------------------------|-----------|---------|-------------------|
| Proto3 full (`make conformance`)      | 707       | 2101    | 0                 |
| Proto2 (`make conformance-proto2`)    | 703       | 2101    | 3                 |
| Proto3 lean (`make conformance-lean`) | 645       | 2101    | 62                |

Each failure in the three `failure_list_proto3.txt` /
`failure_list_proto2.txt` / `failure_list_proto3_lean.txt` files is tagged
with one of the reasons below. The proto3 full suite has no expected failures at all;
proto2's remaining 3 are group-related, and lean's 62 are much more diverse.

- **(groups) proto2.** PPB rejects wire types 3/4 (`StartGroup`/`EndGroup`) with
  `CORRUPT_TAG` by design. The proto2 schema is generated with
  `--ppb_opt=drop_group_extension_fields`, which silently drops group-typed field
  declarations from the schema. However, payloads that *contain* group tag bytes
  still cause PPB to return a parse error. Three proto2 tests that exercise the
  `MessageSet` wire format (which encodes items as groups) or a group extension
  field are expected to fail. This does not affect the proto3 suite, since proto3
  doesn't have groups.

- **proto2 extensions (skipped/retained as unknown).** The translator does not model
  proto2 extension ranges or extension definitions; these are silently logged and
  dropped from the schema via `--ppb_opt=drop_group_extension_fields`. Extension field
  bytes that appear on the wire are therefore handled by the always-on
  unknown-field retention handler, which writes them to the `UnknownFieldSet`.
  This is observably correct except in conjunction with groups, as explained
  above.

- **Lean strictness.** The lean suite has 62 expected failures vs. full's 0.
  All 62 break down as:
  - **(packing) 14 Required non-default wire encoding failures.** Lean emits both
    wire forms for repeated scalars but marks the non-canonical one
    `field_semantics::error` (packed is canonical in proto3). A repeated field that
    arrives in its off-canonical encoding is therefore rejected at `parse()` (this
    includes `ValidDataRepeated.SINT32.UnpackedInput`, rejected before any value is
    decoded). default/full emit the non-canonical form as `always_lexn` and accept
    either encoding; lean rejects it by design (that's why lean exists).
  - **(packing, Recommended) 42 Recommended cross-encoding failures.** With
    `--enforce_recommended`, the 3 output-variant forms (UnpackedInput.DefaultOutput,
    UnpackedInput.PackedOutput, PackedInput.UnpackedOutput) * 14 scalar types are
    promoted to required passes. Lean fails all 42 for the same non-canonical encoding
    reason.
  - **(unknown-retention) 2 failures.** Lean has no catch-all unknown-field
    handler, so `UnknownOrdering` and `UnknownVarint` fail.
  - **(field-number-range) 2 failures.** PPB treats a field number above the
    2^29-1 maximum as an unknown field rather than rejecting it; lean has no
    catch-all handler that can bound check, so `BadTag_FieldNumberTooHigh`
    and `BadTag_FieldNumberSlightlyTooHigh` are silently accepted.
  - **(merge) 2 failures.** Tagged as merge tests, but lean-only for the same
    packing reason: the merged messages contain unpacked repeated scalar
    fields, which lean rejects.

## Divergences tolerated by the byte fuzzers

The byte-level corruption fuzzers (`fuzz_sink_bytes`, `fuzz_sink_mutate`)
compare the full-mode reflection sink against libprotobuf on arbitrary and
corrupted bytes. Their oracle (`fuzz/reflection_differential.hpp`) tolerates
three divergence categories; everything else traps.

- **Overlong framing varints.** libprotobuf rejects tag and length varints
  longer than 5 encoded bytes; PPB decodes framing varints like any other (an
  overlong tag becomes an unknown field, an overlong length is decoded
  normally), so inputs whose framing varints span 6-10 bytes parse with PPB
  and fail with libprotobuf. One fuzzer variant (`NO_OVERLONG`) avoids
  generating such inputs and runs with this tolerance disabled.
- **Non-canonical encoding disagreements.** When both parsers accept the same
  bytes but decode different values, or PPB rejects bytes libprotobuf
  accepts, the divergence is tolerated only when PPB decodes libprotobuf's
  canonical re-serialization back to the reference message; a misdecode of a
  canonically encoded value would corrupt that round-trip and still trap.
  Known instances: tag varints above 32 bits (libprotobuf truncates before
  validating, the sink rejects the out-of-range field number) and groups
  retained in libprotobuf's unknown-field set (PPB rejects wire types 3/4
  outright).
- **Reparse survival.** When PPB accepts bytes libprotobuf rejects, the
  result is tolerated only when re-serializing PPB's decoding still fails
  libprotobuf's parse: the malformed structure survived as-is instead of
  being laundered into a different valid value.
