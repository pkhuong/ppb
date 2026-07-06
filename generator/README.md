# protoc-gen-ppb

`protoc-gen-ppb` is a `protoc` plugin that translates `.proto` files into
header-only [`ppb::schema`](../include/ppb/ppb.hpp) declarations: one
compile-time schema per message, plus a field-key enum and a static
`max_depth`.  The schema also comes with C++ definitions for enums (nested
or toplevel).

## Requirements

- `protoc`
- [`uv`](https://docs.astral.sh/uv/) (manages the Python deps; **always run Python
  here through `uv`**, never a bare `python3`)

The plugin is a single self-contained script, `protoc_gen_ppb.py`, with a uv-script
shebang and PEP 723 inline dependency metadata, so `protoc` can exec it directly.

## Usage

```sh
protoc --plugin=protoc-gen-ppb=generator/protoc_gen_ppb.py \
       --proto_path=path/to/protos \
       --ppb_out=gen \
       my.proto
```

`--ppb_out=<dir>` is the destination directory for the `<base>.ppb.hpp` headers
(one per input `.proto`).  Downstream code only needs `#include "my.ppb.hpp"` and `-I<dir>`.

### Options (`--ppb_opt=`, comma-separated)

`mode=` is a convenience preset that expands to two independent flags; the remaining
options are standalone booleans (off by default unless noted).

| Option | Default | Effect |
|---|---|---|
| `mode=none\|lean\|full` | `lean` | Shorthand preset that expands to the two independent flags below. `lean` sets `strict_repeated_encoding` (reject unexpected wire encodings, skip unknowns). `none` clears both (accept either wire form, skip unknowns). `full` is `none` plus `detect_unknown` (accept either wire form, detect unknowns). |
| `strict_repeated_encoding` | on | Reject unexpected repeated wire encodings at parse time (fallback has `field_semantics::error`). When off, unexpected forms have `field_semantics::always_lexn` instead, forcing a wire-order lexn pass so messages with both packed and unpacked encodings decode in wire order. The flag can override a mode preset: `mode=none,strict_repeated_encoding` relaxes then re-asserts strictness. Already on by default, so passing it alone is idempotent. |
| `detect_unknown` | off | Append `ppb::detect_unknown_fields<>` to every schema, so unknown fields can be reported rather than skipped (e.g. `mode=lean,detect_unknown` rejects alt wire forms *and* detects unknowns). Set by `mode=full`. |
| `opaque_cycles` | off | Allow recursive (cyclic) message graphs by emitting the cycle's back-edge fields as opaque byte spans (see Limitations). |
| `oneof_as_optional` | off | **Lossy, opt-in.** Decode each member of a user-declared oneof as an independent field with `always_lexn` semantics: every member occurrence dispatches, in wire order, so the caller can reconstruct which member won, but oneof exclusivity is not enforced at the schema level. Without this flag, any oneof is rejected with a diagnostic. |
| `drop_foreign_type_fields` | off | **Lossy, opt-in.** Drop any field whose type comes from a well-known-type import (`google/protobuf/*`) that PPB does not support. Dropped fields are listed as `ppb-dropped:` comments at the top of the generated header and are not decoded. Without this flag, fields referencing unsupported well-known types are rejected with a diagnostic. |
| `drop_group_extension_fields` | off | **Lossy, opt-in.** Drop field declarations whose wire type or feature is not supported by PPB (currently: proto2 `group` fields and proto2 extension ranges/definitions). Dropped group fields are listed as `ppb-dropped:` comments; extension ranges and definitions are logged as generator warnings and ignored. Without this flag the generator rejects unsupported declarations with a diagnostic. |
| `always_dispatch_strings` | off | Dispatch every singular `string` field to its handler. Gives users one callback per value (useful for validation). |

To compare wire policies, generate into separate directories and diff:

```sh
protoc --plugin=protoc-gen-ppb=generator/protoc_gen_ppb.py --proto_path=p --ppb_opt=mode=none --ppb_out=gen/none my.proto
protoc --plugin=protoc-gen-ppb=generator/protoc_gen_ppb.py --proto_path=p                     --ppb_out=gen/lean my.proto
protoc --plugin=protoc-gen-ppb=generator/protoc_gen_ppb.py --proto_path=p --ppb_opt=mode=full --ppb_out=gen/full my.proto
```

### Standalone usage (no protoc plugin protocol)

The script also runs as an ordinary command, reading a serialized descriptor set
instead of the protoc plugin stdin protocol. Build the set with protoc, then point
the generator at it:

```sh
protoc --descriptor_set_out=set.pb --include_imports --proto_path=p my.proto other.proto
uv run protoc_gen_ppb.py --descriptor-set set.pb --out gen my.proto other.proto
```

`--include_imports` is required: the descriptor set must include the full import
closure of every file named on the command line, or the generator will fail. The
trailing arguments are the files to generate headers for (the `file_to_generate` list);
`--out` is the output directory and `--opt` takes the same comma-separated options as
`--ppb_opt`. A few supported well-known types are injected automatically, so a descriptor
set need not (but may) include `google/protobuf/*.proto`.

```sh
uv run protoc_gen_ppb.py --emit-wkt-bundle include/ppb/wkt.ppb.hpp
```

`--emit-wkt-bundle <path>` generates the `<ppb/wkt.ppb.hpp>` bundle (the fused
schemas for the six supported well-known types) without any input descriptor set: the
six well-known type definitions are read from the plugin's protobuf runtime.

## Well-known types

Six well-known types decode normally; a field referencing one resolves like an
ordinary cross-file message reference, and results in `#include <ppb/wkt.ppb.hpp>`:

- `google/protobuf/any.proto`
- `google/protobuf/duration.proto`
- `google/protobuf/empty.proto`
- `google/protobuf/field_mask.proto`
- `google/protobuf/timestamp.proto`
- `google/protobuf/wrappers.proto` (the `*Value` wrappers)

The bundle lives in `include/ppb/wkt.ppb.hpp` and is regenerated with
`--emit-wkt-bundle` (see [Standalone usage](#standalone-usage-no-protoc-plugin-protocol)).
Headers that reference one of these types emit `#include <ppb/wkt.ppb.hpp>` once, in
place of a per-file include. An exception is made for supported well-known files that
are listed in `file_to_generate`: explicitly requested output files are generated normally.

The remaining well-known types are not decoded: `Struct`/`Value`/`ListValue`,
`Type`/`Api`, `SourceContext`, and the `descriptor.proto` messages. `Struct`,
`Value`, and `ListValue` are mutually recursive and would need `opaque_cycles`. A
field referencing any of these is rejected with a diagnostic naming the flag, or,
with `--ppb_opt=drop_foreign_type_fields`, dropped (listed as `ppb-dropped:`
comments at the top of the header and not decoded).

Classification is by name, not by content: any field whose type lives under
package `google.protobuf` -- including a user-defined message that merely
declares that package -- is treated as a well-known-type reference, and
rejected or dropped like the unsupported types above unless it comes from one
of the six supported files. Similarly, imported files under the
`google/protobuf/` path outside the supported list are skipped wholesale
(files explicitly listed for generation are still generated).

## Generated layout

Each message `Foo` in package `pkg` emits a C++20 namespace nested under a dedicated
toplevel `ppb_gen` namespace:

```cpp
namespace ppb_gen::pkg::Foo
{
    enum class F : ::std::int32_t      // field numbers (IDL order)
    {
        x = 1,
        sub = 3,
    };

    constexpr ::std::size_t max_depth = 1;   // longest submessage nesting

    using schema = ::ppb::auto_schema<
        // ppb::on<F::x>(...)
        ::ppb::proto3_int32<F::x>,
        // ppb::on_submessage<F::sub, ::ppb_gen::pkg::Bar::merge_schema>(...)
        ::ppb::message<F::sub, ::ppb_gen::pkg::Bar::merge_schema, ::ppb::field_semantics::singular>>;

    using merge_schema = ::ppb::auto_schema<
        // ppb::on<F::x>(...)
        ::ppb::int32<F::x>,
        // ppb::on_submessage<F::sub, ::ppb_gen::pkg::Bar::merge_schema>(...)
        ::ppb::message<F::sub, ::ppb_gen::pkg::Bar::merge_schema, ::ppb::field_semantics::singular>>;
}
```

- Everything generated lives under `ppb_gen` so a `package pkg` message `Foo` becomes
  `ppb_gen::pkg::Foo`. `ppb_gen` is a *sibling* of the `ppb` library namespace, not
  nested inside it (a proto package named `schema`/`reader` would otherwise collide
  with `ppb::schema` / `ppb::reader`).
- The field-key enum is `F`; descriptors look like `ppb::int32<F::x>`.
- Nested message `Foo.Bar` become `namespace ppb_gen::pkg::Foo::Bar`, referenced as
  `::ppb_gen::pkg::Foo::Bar::schema`. Toplevel enums are emitted in the package namespace;
  nested enums in their message's namespace.
- `max_depth` is a static constant you can pass to `ppb::limit::max_depth`.
- If a proto contains a type literally named `F`, `schema`, `merge_schema`, or `max_depth`,
  the injected identifier is deterministically renamed (`F` -> `F_`, etc.).

**Merge is unconditional.** A singular (non-repeated) message field dispatches every
wire occurrence and merges them field-by-field, matching protobuf's message-merge
semantics for a message split across occurrences. Each message therefore gains a
`merge_schema` alias alongside `schema`: a **singular** message field references the
child's `merge_schema`, because repeated wire occurrences of one singular submessage
merge into a single child. A proto3 scalar absent from a later occurrence would then
be dispatched as zero and overwrite a value an earlier occurrence set, so `merge_schema`
uses the last-write-wins, non-`proto3_` scalar variants (absent scalars are not
dispatched). A **repeated** message field (including map entries) instead gets a fresh
child per occurrence, so the plain `schema` with proto3 zero-defaults is correct.

### Empty messages

A message with no fields (e.g. `message Empty {}` or `google.protobuf.Empty`)
would otherwise emit an empty `ppb::auto_schema<>`, which is ill-formed:
it must have at least one descriptor. Such messages instead register only
`ppb::detect_unknown_fields<>` (regardless of mode or `detect_unknown`),
keeping the schema valid, and the generator prints a warning per empty message. If
you only need to know whether such a submessage was present, prefer
`ppb::on<F::field>()` in the containing message to receive its raw span: that gives
presence/absence detection without descending into the submessage. Handle unknown
fields inside the empty message only when you must inspect its contents.

### Field mapping (summary)

- `int32/uint32/sint*/bool/...`: the matching `ppb` scalar; proto3 implicit-presence
singular scalars use the `proto3_*` (zero-default) aliases, while proto2 fields,
proto3 explicit `optional`, and message fields use last-write-wins. Proto2
explicit `default = ...` values are never applied (see
[Proto2 semantics](#proto2-semantics)).
- `enum`: `ppb::enumerated<K, Enum>` over a generated scoped `enum class`
(`proto3_enumerated` for proto3 implicit presence). Decoding is always
open-enum, even for closed proto2 enums (see
[Proto2 semantics](#proto2-semantics)).
- `string`/`bytes`: `utf8string`/`bytes` (with `proto3_*` variants for proto3 implicit presence).
- `message`: `ppb::message<K, Inner::merge_schema, singular>` for
a singular field, `ppb::unpacked_message<K, Inner::schema>` for a repeated one.
- `map<K, V> field = N`: protoc lowers each map into a synthetic nested message named
after the CamelCased field name plus `Entry` (e.g. `my_field` -> `MyFieldEntry`), with
`key = 1` and `value = 2` fields, plus a repeated field referencing it. The generator
emits this as an ordinary repeated message field
(`ppb::unpacked_message<F::field, ::pkg::Foo::MyFieldEntry::schema>`) where the entry
gets its own namespace with `key`/`value` typed normally (proto3 zero-default aliases
apply inside a proto3 entry).

Repeated scalars/enums emit both wire forms per the `mode` rule above; repeated
`string`/`bytes`/`message` are always `unpacked_*`.

### Proto2 semantics

Three proto2 behaviors are deliberately not reproduced. The generator prints a
warning per affected field and otherwise decodes it like its proto3
counterpart:

- **Explicit defaults are not applied.** A field's `default = ...` value
  never reaches the handler: an absent field simply doesn't dispatch, with or
  without a declared default. Apply defaults yourself, e.g. by initializing
  destination values before `parse()`; the warning reports each declared
  default value.
- **Closed enums decode as open.** The generated descriptor dispatches the
  raw wire value, so an out-of-range value on a closed (proto2) enum reaches
  the handler as its bit pattern instead of being routed to the unknown-field
  set as the spec requires. Range-check in the handler if you need closed
  semantics. Closedness follows the enum's defining file, so this also
  applies to proto3 messages that reference a proto2 enum.
- **`required` is not enforced.** Required fields decode like optional ones;
  presence checking is left to the caller.

## Limitations (rejected with a diagnostic)

- **Recursion**: proto message reference cycles can't be expressed as
  `ppb::schema` aliases (the wrapper's `reader<>` only accepts a literal
  `schema<...>`, and `using` aliases can't be forward-declared). By default a cycle
  is **rejected**; `--ppb_opt=opaque_cycles` converts the cycle's back-edge fields
  to opaque byte spans so the rest compiles.
- **Groups** (proto2 `group`): PPB rejects wire types 3/4.
- **Extensions**: rejected.
- **oneof**: oneofs are rejected by default. Use `--ppb_opt=oneof_as_optional` to opt in
  to lossy decoding: each member is emitted as an independent `always_lexn` field, so
  handlers fire per occurrence in wire order (the last member dispatched is the one
  that won), but oneof exclusivity is not enforced at the schema level.
- **Editions**: only `proto2`/`proto3` syntax is supported.
- **Nesting depth**: message declarations nested more than 2000 levels deep
  are rejected with a diagnostic (the plugin's protobuf runtime caps descriptor
  decoding; protoc itself accepts deeper files). Deeply nested schemas also
  produce headers with unacceptable compile times and RAM footprint.

## Compile cost

The generated headers trade C++ compile time for runtime dispatch: schemas are
template metaprograms, so unusually wide or deep messages cost real compiler
time and memory (measured with g++ 16 at `-O1`):

- **Fields per message**: up to ~1,000 fields per message compiles with stock
  compiler limits (a 1,000-field message takes >20 s and >1 GB). Cost grows
  superlinearly with field count; a 10,000-field message exceeds several
  default g++ limits (`-ftemplate-depth`, `-fconstexpr-ops-limit`) and then
  needs over 10 GB of RAM.
- **Nesting depth**: a handler chain around ~85 `on_submessage`
  levels deep exceeds g++'s default `-ftemplate-depth=900` (each nesting level
  costs roughly ten); raise the flag for deeper chains (a 100-level chain
  compiles in under 10 s with `-ftemplate-depth=1200`).

## Development

Look at `generator/Makefile` (everything runs through `uv`):

```sh
cd generator
make check        # ruff format-check + ruff lint + pyrefly + pytest
make format       # ruff format (rewrite in place)
make typecheck    # pyrefly
make test         # pytest
make regen        # regenerate testdata/golden/*.ppb.hpp
```

From the repo root, `make generator_test` runs `make -C generator check`, verifies the
golden headers are up to date, and compiles the generated schemas against `ppb.hpp`
(skips cleanly if `protoc`/`uv` are unavailable).
