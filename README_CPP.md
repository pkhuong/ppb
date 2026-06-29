`ppb::reader`: C++ wrapper for PPB
===================================

`<ppb/ppb.hpp>` is a header-only C++20 wrapper around the C lexer in
`<ppb/ppb.h>`.  It compiles a typed schema into the encoded-tag arrays
the C library consumes, then dispatches matched fields to
caller-supplied handlers.  Like the C library, it never allocates and
instead borrows the input bytes: callers pass a byte span and decoded
values either point back into that span or are returned by value.

When to use this wrapper
------------------------

**Use `ppb::reader` when** you parse protobuf from an untrusted
producer (the C lexer underneath is formally verified against
adversarial bytes), when every heap allocation should be one your code
asked for, when you want to right-size each output container before
any field is dispatched, when you have several message types and want
the compiler help you keep track of which you're parsing, and when
you're already on a modern (GCC/clang, C++20) toolchain.

**Look elsewhere when** you also need to encode protobuf, when you
need full protobuf features (groups, runtime descriptors, reflection,
first-class maps), when `.proto` files are your source of truth and
code generation fits your workflow
([nanopb](https://github.com/nanopb/nanopb), Google's libprotobuf), or
when MSVC support matters.  See `README.md` for the decode-side
comparison and encoding recommendations.

If `.proto` files are your source of truth but you want this wrapper
anyway, see [generator/README.md](generator/README.md): `protoc-gen-ppb`
is a `protoc` plugin that generates the `ppb::schema` declarations shown
below directly from `.proto` files.

The wrapper itself never calls `operator new`: the reader's per-field
state is a `std::array<ppb_field, N>`, handler tuples live on the
stack, and submessage recursion uses stack frames.  When *your* code
needs output storage, the `init` callback runs after prescan with
`meta<Key>()` populated, so you can `reserve()` each container to its
final size before any handler fires:

```cpp
return r.parse(
    [&](const auto &snap) -> ppb_error {
        out.reserve(snap.meta<F::items>().num_occurrences);
        return PPB_OK;
    },
    ppb::push_back<F::items>(&out));
```

The `init` callback must return `ppb_error`; a non-OK return value
aborts parsing early.

To preserve the no-useless-allocation property in your own code,
pre-`reserve()` every container, prefer `std::string_view` over
`std::string`, and don't type-erase handlers with `std::function<>`
(it may allocate; the wrapper takes lambdas by template).

Read the C API docs first (in `ppb.h` and the toplevel `README.md`)
for the prescan-then-lex model, limits, and error codes.  The wrapper
performs the same validation `ppb_validate_tags` does at runtime, so
the C check is not needed.  Pay attention to the "Gotchas, decoding
quirks, and footguns" section.

The C library is formally verified and fuzzed; the C++ wrapper has
unit tests and a fuzz target.  The wrapper's dispatching logic sits
above the verified C boundary, so any bug there is outside the proofs.

The C lexer runs in Θ(n log m) for n toplevel fields against an
m-field schema.  Handler dispatch is a linear walk over the schema,
so the wrapper's worst case is Θ(m · n) with a small constant.

Quick start
-----------

```cpp
#include <ppb/ppb.hpp>

// One `enum class` per message type keeps handlers from being applied
// to readers for a different schema.
enum class MyMessage : int {
    count   = 1,
    name    = 2,
    payload = 3,
};

using Schema = ppb::schema<
    ppb::int32     <MyMessage::count>,
    ppb::utf8string<MyMessage::name>,
    ppb::bytes     <MyMessage::payload>>;

ppb_error parse_message(std::span<const std::byte> bytes)
{
    int32_t count = 0;
    std::string_view name;
    std::span<const std::byte> payload;

    ppb::reader<Schema> r(bytes);

    return r.parse(
        ppb::store<MyMessage::count>  (&count),
        ppb::store<MyMessage::name>   (&name),
        ppb::store<MyMessage::payload>(&payload));
}
```

Typed scalar fields default to `last_write_wins`, so `store` captures
only the last occurrence.  Use the `unpacked_*` aliases with
`push_back` / `emplace_back` when you need every occurrence; use a
packed field when the scalar can be encoded that way.

Schemas
-------

A `ppb::schema<Fs...>` is a type list of field descriptors validated
at compile time:

```cpp
using S = ppb::schema<ppb::varint<1>, ppb::len<2>, ppb::i32<3>>;
```

- At least one field.
- All fields share the same `Key` type (one `enum class` per message
  is recommended).  `ppb::unknown<wire>` catch-alls carry no key and
  are exempt.
- Fields are listed in strictly ascending encoded-tag order.  When
  the same field number appears under multiple wire types, list them
  in `enum ppb_wire_type` order (varint, i64, len, i32).  Catch-alls
  encode as if their field number were the maximum possible value,
  so any `ppb::unknown<wire>` entries must come after every regular
  field; mixing them into the middle of the list fails the
  ascending-order check.  `ppb::auto_schema<...>` reorders for you.

### `ppb::auto_schema<...>`: flatten and sort

`ppb::auto_schema<Ts...>` accepts field descriptors and/or (possibly
nested) `std::tuple<...>`s of descriptors, flattens, sorts by
`(field_number, wire_type)`, and yields the corresponding
`ppb::schema<...>`.  Useful when composing a schema from reusable
sub-tuples:

```cpp
using shared_fields = std::tuple<ppb::varint<3>, ppb::len<7>>;
using my_schema = ppb::auto_schema<
    ppb::varint<1>,
    shared_fields,
    ppb::i32<2>>;
// == ppb::schema<ppb::varint<1>, ppb::i32<2>, ppb::varint<3>, ppb::len<7>>
```

Duplicate `(field_number, wire_type)` pairs are still rejected.

Field types
-----------

### Wire-typed primitives

`ppb::varint<K>`, `ppb::i64<K>`, `ppb::len<K>`, `ppb::i32<K>` describe
fields by wire type only.  Handlers receive `const ppb_field &`.

### Typed scalars

These wrap a wire-typed primitive and decode into a native C++ type:

| Field type                    | Handler argument           | Wire type |
|-------------------------------|----------------------------|-----------|
| `int32<K>` / `int64<K>`       | `int32_t` / `int64_t`      | varint    |
| `sint32<K>` / `sint64<K>`     | `int32_t` / `int64_t`      | varint (zigzag) |
| `uint32<K>` / `uint64<K>`     | `uint32_t` / `uint64_t`    | varint    |
| `boolean<K>`                  | `bool`                     | varint    |
| `enumerated<K, Enum>`         | `Enum`                     | varint    |
| `fixed32<K>` / `fixed64<K>`   | `uint32_t` / `uint64_t`    | i32 / i64 |
| `sfixed32<K>` / `sfixed64<K>` | `int32_t` / `int64_t`      | i32 / i64 |
| `f32<K>` / `f64<K>`           | `float` / `double`         | i32 / i64 |
| `utf8string<K>`               | `std::string_view`         | len       |
| `bytes<K, Element=std::byte>` | `std::span<const Element>` | len       |
| `message<K, InnerSchema>`     | (submessage, see below)    | len       |

`enumerated<K, Enum>` does **not** range-check: out-of-range wire
values reach the handler as the corresponding bit pattern.  `Enum`
must therefore have a fixed underlying type (a scoped enum or an
explicit underlying type) because otherwise casting an out-of-range
integer to the enum could be undefined behavior in C++.  The wrapper
enforces this at compile time; plain unscoped enums are rejected with
a `static_assert` on the `ppb::detail::fixed_underlying_enum` concept.

`utf8string<K>` passes the handler a `std::string_view` over the
payload bytes.  Protobuf requires UTF-8, but the wrapper does **not**
validate the encoding; callers who care must check themselves.

`bytes<K, Element>` requires `alignof(Element) == 1`.  Payloads whose
length is not a multiple of `sizeof(Element)` set the reader's error
to `PPB_ERROR_TRUNCATED_DATA` and dispatch an empty span.

`message<K, InnerSchema>` is a LEN field whose payload is a
protobuf-encoded sub-message of type `InnerSchema`.  Pair it with
`ppb::on_submessage<K, InnerSchema>(...)`, which decodes the payload
through a nested `reader<InnerSchema>` and statically checks that the
schemas match (see **Embedded sub-messages**).  A plain
`ppb::on<K>(std::span<const std::byte>)` handler also binds: the
handler receives the raw payload and can construct its own inner
reader (manual inner-reader form).

### Packed and unpacked repeated fields

- **Packed fixed-width:** `packed_fixed32`, `packed_sfixed32`,
  `packed_f32`, `packed_fixed64`, `packed_sfixed64`, `packed_f64`.
  Handlers receive `std::span<const ppb::le_packed<T>>`;
  `le_packed<T>` is a packed unaligned little-endian wrapper.
  Convert to host order with `static_cast<T>(elem)` or `elem.value()`.
- **Packed varint:** `packed_int32`, `packed_int64`, `packed_sint32`,
  `packed_sint64`, `packed_uint32`, `packed_uint64`, `packed_boolean`,
  `packed_enumerated<K, Enum>`.  Handlers receive a lazy view that
  decodes one varint per `++`:

  ```cpp
  ppb::on<F::indices>([](auto view) -> ppb_error {
      for (int32_t v : view)
          process(v);
      return PPB_OK;
  })
  ```

  Decode failures (truncation, corrupt varint) set the reader's error
  and exhaust the iterator.

- **Unpacked repeated:** `unpacked_int32`, `unpacked_utf8string`,
  `unpacked_bytes<K, Element>`, `unpacked_enumerated<K, Enum>`, ...
  Aliases for the matching scalar with `field_semantics::repeated`;
  the handler fires once per occurrence.

To accept either encoding for the same field number, declare both:

```cpp
using S = ppb::auto_schema<
    ppb::packed_int32  <F::indices>,
    ppb::unpacked_int32<F::indices>>;
```

A single `ppb::push_back<F::indices>(&out)` handles both: the wrapper
matches by `(Key, wire)`, the packed branch iterates the view, and the
unpacked branch appends one `int32_t` per occurrence.  To dispatch the
two wire types separately, bind a wire type with the second template
argument:

```cpp
r.parse(
    ppb::on<F::indices, ppb::wire_type::len>(
        [&](auto view) -> ppb_error { /* packed payload */ return PPB_OK; }),
    ppb::on<F::indices, ppb::wire_type::varint>(
        [&](int32_t v) -> ppb_error { /* one occurrence */ return PPB_OK; }));
```

### Field semantics

Every scalar accepts an optional `field_semantics` template parameter
that controls dispatch when the same field appears multiple times:

| `field_semantics`     | Behavior |
|-----------------------|----------|
| `repeated`            | Dispatch every occurrence. |
| `singular`            | Last-write-wins when the squashed values are bit-identical (varint/i32/i64); otherwise per-occurrence.  LEN payloads aren't compared, so any LEN repeat is per-occurrence. |
| `last_write_wins`     | Only the last occurrence is dispatched.  Default for typed scalars; matches proto2 optional scalar semantics. |
| `always_lexn`         | Per-occurrence dispatch even with a single occurrence (preserves wire order).  Default for raw `varint`/`i64`/`len`/`i32`. |
| `proto3_zero_default` | Like `last_write_wins`, plus: absent fields dispatch with their zero value (or empty span) when the schema takes the prescan fast path.  See below. |
| `error`               | Any occurrence during `parse()` is an error (see "Error reporting"). |

Packed fields are always `repeated`.

Absent-field synthesis under `proto3_zero_default` only fires on the
prescan fast path, and only when `parse()` actually reaches the
dispatch step.  If any other field in the schema forces a lexn pass
(e.g. a `repeated` field), absent `proto3_zero_default` fields are
*not* dispatched.  An empty input (or any other early-exit from
`parse()` -- prescan returning zero bytes, `init` returning non-OK,
a `field_semantics::error` field on the wire) skips dispatch
entirely, so zero defaults don't fire there either.  Handling a zero
default must therefore yield the same result as not handling the
field at all (i.e., leaving it at its initial value).

#### Proto3 optional semantics

The `proto3_*` aliases are the shorthand for typed scalars with
`proto3_zero_default`:

```cpp
enum class Msg : int { id = 1, active = 2, score = 3 };

using MsgSchema = ppb::schema<
    ppb::proto3_uint32 <Msg::id>,
    ppb::proto3_boolean<Msg::active>,
    ppb::proto3_uint32 <Msg::score>>;
```

Full alias list: `proto3_int32`, `proto3_int64`, `proto3_sint32`,
`proto3_sint64`, `proto3_uint32`, `proto3_uint64`, `proto3_boolean`,
`proto3_enumerated<K, Enum>`, `proto3_fixed32`, `proto3_sfixed32`,
`proto3_f32`, `proto3_fixed64`, `proto3_sfixed64`, `proto3_f64`,
`proto3_utf8string`, `proto3_bytes<K, Element=std::byte>`.

There is no `proto3_message` alias: proto3 message fields keep proto2
explicit-presence semantics on the wire, and absent submessages are
not synthesized.  Use plain `message<K, InnerSchema>` for both proto2
and proto3.

### Catch-alls and unknown-field detection

`ppb::unknown<wire>` registers a catch-all per wire type so unknown
tags route to a known bucket instead of being silently skipped.
`ppb::detect_unknown_fields<sem>` is a `std::tuple` of all four variants,
ready to drop into `auto_schema`; `sem` defaults to
`field_semantics::repeated`:

```cpp
using S = ppb::auto_schema<MyFields..., ppb::detect_unknown_fields<>>;
// With `field_semantics::error`: unknown fields become hard errors.
using Strict = ppb::auto_schema<MyFields..., ppb::detect_unknown_fields<field_semantics::error>>;
```

`unknown<wire>` carries no `Key`, so it composes with any user key
type.  Unknown-field activity surfaces in two ways once registered:

- `reader::unknown_field()` returns the encoded tag
  (`field_number * 8 + wire_type`) of an unknown field that hit a
  catch-all.  See **Error reporting**.
- `ppb::on_unknown<wire>(callable)` dispatches per occurrence with
  `const ppb_field &`.  See **Handlers**.

Reading a message
-----------------

```cpp
ppb::reader<Schema> r(bytes);  // or r(ptr, length)
```

The reader holds a byte span, a sticky error, and a per-field state
array sized to the schema.  Trivially copyable: a default-constructed
reader is empty (no input, no error), and copying a live reader
produces an independent snapshot, cheap to checkpoint.

The reader borrows the input bytes; it does not copy them.  The
buffer must outlive every call that reads from this reader **and**
every `std::string_view` / `std::span<const Element>` passed to a
handler: decoded LEN payloads point back into the input.  Numeric
scalars are returned by value and have no such dependency.

### Reader accessors

| Member                | Meaning                                                              |
|-----------------------|----------------------------------------------------------------------|
| `error()`             | Sticky `ppb_error`; once non-`PPB_OK`, every call short-circuits.    |
| `error_field()`       | See **Error reporting**.                                             |
| `unknown_field()`     | See **Error reporting**.                                             |
| `input()`             | Unconsumed suffix of the input span.                                 |
| `size()` / `empty()`  | Shortcuts for `input().size()` / `input().empty()`.                  |
| `reset_fields()`      | Zeroes per-field state, but **not** the input span or any other state.  A reader that has already failed must be overwritten with a fresh reader. |

### `parse()`

```cpp
// Five equivalent shapes; they all forward to the last one:
parse(handlers...);
parse(limit, handlers...);
parse(init, handlers...);
parse(limit, init, handlers...);
parse(init, limit, handlers...);
```

`parse()` runs `ppb_prescan` over the full input (subject to
`limit`), calls `init(std::as_const(*this))`, then dispatches
handlers: once with the prescan-aggregated values (fast path), or
per batch via `ppb_lexn` when any field forces it.  The input span
advances past the consumed bytes on return.

- `init` sees the post-prescan reader, so it can call `meta<Key>()`
  to preallocate.  Pass `std::nullopt` (or omit it) to skip.
- The `handlers...` pack may be empty when an `init` is supplied:
  `parse(init)` or `parse(init, limit)` runs prescan, fires `init`
  with the post-prescan metadata, and returns without dispatching
  any field handlers.  Useful for validation or for sizing output
  containers without decoding.
- A non-`PPB_OK` return from `init` becomes the sticky error; the
  input span is **not** advanced.
- A `field_semantics::error` field on the wire makes `parse()`
  return `PPB_ERROR_CORRUPT_TAG`, sets `error_field()`, and skips
  `init` and the handlers.
- Empty input (or prescan reporting zero bytes) returns `PPB_OK`
  without invoking `init` or any handler; even `proto3_zero_default`
  fields don't fire their handlers.
- Handler errors fold into the sticky error first-write-wins, but
  every handler in the current batch still runs: side effects from
  other handlers in the batch are not rolled back.
- Handler call order across a batch is **not specified**.  If two
  handlers' side effects depend on each other's, merge them into a
  single handler rather than relying on wire order, schema order,
  or the order of the `on<>` arguments to `parse()`.

Registering an `on_unknown` handler can force a lexn pass when the
catch-all field appears multiple times on the wire.  Opting into
`detect_unknown_fields` for just the soft `unknown_field()` flag does
not.

### `parse()` with an init callback

`init` runs after prescan, with `meta<Key>()` populated.  The
`reserve()`-from-`meta` pattern shown at the top of this README is
the main use case; `ppb_field_meta` exposes these aggregates:

- `num_occurrences`: count of wire-side occurrences.
- `lost_distinct_u64`: set when distinct varint/i32/i64 occurrences
  were squashed; useful to detect data loss under `singular`.
- `total_bytes`, `min_nonzero_bytes`, `max_bytes`: payload-size
  aggregates (for all wire types).

### Embedded sub-messages

Embedded messages are LEN fields whose payload is a serialized
sub-message.  Declare the outer field as `ppb::message<K, InnerSchema>`
(singular) or `ppb::unpacked_message<K, InnerSchema>` (repeated),
and pass the inner handlers to `ppb::on_submessage<K, InnerSchema>(...)`.
The wrapper builds the inner reader and parses the payload.

A malformed inner message whose fields overrun the declared payload
length is reported as `PPB_ERROR_LIMIT_EXCEEDED` (the payload length
acts as the inner parse's hard byte limit), not
`PPB_ERROR_TRUNCATED_DATA`.

`on_submessage<K, S>` is bound to `wire_type::len` and will match
*any* LEN field at key `K` in the outer schema -- `message<K, S>`,
`unpacked_message<K, S>`, but also `bytes<K>`, `utf8string<K>`,
`len<K>`, `unpacked_bytes<K>`, etc.  Only when the outer field is
`message<K, S2>` or `unpacked_message<K, S2>` does the wrapper
statically check `S == S2`; for any other LEN field the payload is
parsed as `S` with no compile-time guarantee that the bytes are
actually a serialized `S`.  Pairing a plain `bytes`-shaped field with
`on_submessage` is the escape hatch when the inner shape isn't
expressible as a `message<K, S>`; the caller is responsible for
knowing the bytes are well-formed.

Supply `ppb::limit::max_depth(N)` (N > 0) on the outer call.
Submessage traversal fails closed with `PPB_ERROR_DEPTH_EXCEEDED`
otherwise -- and that check fires **at runtime**, when the first
embedded field is encountered, not at compile time.  Forgetting
`max_depth` is silent until a message that actually contains the
embedded field arrives.

Only the depth budget carries into the inner reader (decremented by
one).  The outer call's `max_fields` and byte cap are **not**
propagated: every inner parse gets a fresh `max_fields`-unlimited
limit with a hard byte cap equal to the payload size.  Drop to the
manual-inner-reader form below if the inner message needs its own
caps.

```cpp
enum class Outer : int { header = 1, items = 2 };
enum class Inner : int { id = 1, value = 2 };

using InnerSchema = ppb::schema<
    ppb::uint32<Inner::id>,
    ppb::uint32<Inner::value>>;

using OuterSchema = ppb::schema<
    ppb::utf8string     <Outer::header>,
    ppb::unpacked_message<Outer::items, InnerSchema>>;

ppb_error decode_outer(std::span<const std::byte> bytes,
                       std::vector<Item> &out)
{
    ppb::reader<OuterSchema> r(bytes);
    Item *cur = nullptr;

    return r.parse(
        [&](const ppb::reader<OuterSchema> &snap) -> ppb_error {
            out.reserve(snap.meta<Outer::items>().num_occurrences);
            return PPB_OK;
        },
        ppb::limit::max_depth(1),
        ppb::on<Outer::header>([](std::string_view sv) -> ppb_error {
            use_header(sv);
            return PPB_OK;
        }),
        ppb::on_submessage<Outer::items, InnerSchema>(
            /* init: emplace before inner field handlers run. */
            [&](const ppb::reader<InnerSchema> &) -> ppb_error {
                cur = &out.emplace_back();
                return PPB_OK;
            },
            ppb::on<Inner::id>   ([&](uint32_t v) -> ppb_error { cur->id    = v; return PPB_OK; }),
            ppb::on<Inner::value>([&](uint32_t v) -> ppb_error { cur->value = v; return PPB_OK; })));
}
```

`on_submessage` invokes its init callback with the inner reader
after its prescan, so inner `meta<Key>()` is available for
per-entry preallocation.  The `emplace_back`-in-init pattern works
because the outer init reserved capacity first and only one inner
parse runs at a time.

#### Manual inner reader

When the outer field is a plain `bytes<K>` or `unpacked_bytes<K>` (no
inner schema), the handler receives `std::span<const std::byte>` and
can construct its own inner reader.  Use this form when you need to
run code *after* the inner parse completes:

```cpp
ppb::on<Outer::items>([&out](std::span<const std::byte> inner_bytes) -> ppb_error {
    Item item;
    ppb::reader<InnerSchema> r(inner_bytes);
    if (auto err = r.parse(
            ppb::on<Inner::id>   ([&](uint32_t v) -> ppb_error { item.id    = v; return PPB_OK; }),
            ppb::on<Inner::value>([&](uint32_t v) -> ppb_error { item.value = v; return PPB_OK; }));
        err != PPB_OK)
        return err;
    out.push_back(std::move(item));
    return PPB_OK;
})
```

Either form receives a span into the original input buffer.

### Streaming: `lex_all()` without upfront prescan

`parse()` validates the entire message before invoking any handler.
For long streams of repeated sub-messages (or any case where the outer
container is too large to prescan up front) use `lex_all()`, which
drives `lexn()` in a loop until the input is drained.  `on_submessage`
works the same way.

Unlike `parse()`, `lex_all()` skips the upfront whole-input prescan:
the outer reader's `meta<Key>()` and `unknown_field()` are **not**
populated, and encoding errors surface per batch rather than upfront.
The signature is otherwise close enough to `parse()` to invite mix-ups;
if you need either of those properties, use `parse()` (or `prescan()`
first, then `lex_all()`).

Also, unlike `parse()`, `limit` applies to each individual `lexn`
call.  A `limit` that forbids progress (byte or field limit of zero)
returns immediately with the current sticky error (or `PPB_OK` if
none).  Otherwise, the loop runs until the input is exhausted or an
error is generated (by `ppb_lexn` or one of the handlers).

```cpp
using OuterSchema = ppb::schema<
    ppb::unpacked_message<Outer::items, InnerSchema>>;

ppb_error stream_items(std::span<const std::byte> stream_bytes,
                       std::vector<Item> &out)
{
    ppb::reader<OuterSchema> stream(stream_bytes);
    Item *cur = nullptr;

    return stream.lex_all(
        ppb::limit::max_depth(1),
        ppb::on_submessage<Outer::items, InnerSchema>(
            [&](const ppb::reader<InnerSchema> &) -> ppb_error {
                cur = &out.emplace_back();
                return PPB_OK;
            },
            ppb::on<Inner::id>   ([&](uint32_t v) -> ppb_error { cur->id    = v; return PPB_OK; }),
            ppb::on<Inner::value>([&](uint32_t v) -> ppb_error { cur->value = v; return PPB_OK; })));
}
```

Trade-offs vs. `parse()`:

- Processing starts immediately; an early exit stops without scanning
  the rest.
- Encoding errors inside the outer message surface per batch, not
  upfront.
- The outer reader's `meta<Key>()` and `unknown_field()` are not
  populated.  Each inner parse still prescans its sub-message, so
  inner `meta<>` is available in the `on_submessage` init.

If you need outer metadata *and* per-occurrence dispatch, call
`prescan()` first, then `lex_all()`.

### Lower-level entry points

- `prescan(limit, handlers...)`: runs `ppb_prescan` and dispatches
  handlers once with the aggregated values.  Returns a `ptrdiff_t`
  byte count (>= 0) or a negative `ppb_error`.  Does *not* advance
  the input span.  When handlers are passed, absent
  `proto3_zero_default` fields dispatch with their zero value,
  matching `parse()`'s fast-path behavior; `lexn()` does not.
- `lexn(limit, handlers...)`: runs one `ppb_lexn` batch, dispatches,
  and advances the input span.  Does not interpret `field_semantics`:
  every occurrence dispatches, and `error` semantics are not enforced.
  Advances the input span unconditionally, including on non-fatal
  limit errors; the consumed bytes are not recoverable.
- `lex_all(limit, handlers...)`: drains the input with repeated
  `lexn` batches.  `limit` applies *per batch*, not cumulatively.
- `dispatch(handlers...)`: re-dispatches against the current
  per-field state without touching the input span.  Mostly for
  tests.  Unlike `parse()`'s fast path and `prescan(handlers...)`,
  `dispatch()` does **not** synthesize absent `proto3_zero_default`
  fields: only fields whose `field.v.ptr` is non-null fire.

### `meta<Key, wire>()`

Returns the `ppb_field_meta` aggregated by the most recent prescan.
Omitting `wire` merges across all schema entries that share `Key`.
A compile error fires when nothing matches.

Handlers and `ppb::on<>()`
--------------------------

```cpp
ppb::on<Key, wire = wire_type::any>(callable)
```

- `Key` must match the schema's `Key` type.  Mixing an `enum class`
  with raw integers (in either direction) is a compile error.
- When several handlers match the same `(Key, wire)`, the wrapper
  picks the one whose argument type matches the field's decoded
  value.  Zero or ambiguous matches are compile errors.
- Handlers return `ppb_error`.

### Handler shortcuts

Three factories cover the common destination patterns:

```cpp
ppb::store<Key>(&destination)        // last-write-wins assignment
ppb::push_back<Key>(&container)      // append via push_back
ppb::emplace_back<Key>(&container)   // append via emplace_back
```

The destination type is deduced from the pointer; the pointee must
outlive the `parse()`/`prescan`/`lexn()`/`dispatch()` call.

When the matched field is a packed view
(`ppb::packed_*` or a `std::span<const le_packed<T>>`), `push_back` /
`emplace_back` iterate the view and append each element individually
rather than appending the view itself.

### `ppb::on_unknown<>()`

```cpp
ppb::on_unknown<wire = wire_type::any>(callable)
```

Fires once per occurrence of any catch-all (`ppb::unknown<wire>`)
field the schema registered.  The handler receives `const ppb_field &`;
`field.v.ptr` points at the original tag byte, so callers who need
the wire-side field number can recover it with `ppb_decode_varint`.

Specify a concrete `wire` to bind to one bucket.  Mixing per-wire
`on_unknown` factories with `ppb::on<Key>` handlers in the same
`parse()` call is supported.  Ambiguity rules mirror `ppb::on<>`.

Error reporting
---------------

The reader carries a single sticky `ppb_error`:

- `error()`: current error.  Once non-`PPB_OK`, every subsequent
  `prescan`/`lexn`/`parse`/`dispatch` short-circuits.
- `error_field()`: `optional<uint64_t>` set when a
  `field_semantics::error` field appeared on the wire.  Holds the
  encoded tag (`field_number * 8 + wire_type`).  Recover the field
  number with `tag / 8` and the wire type with `tag % 8`
  (`static_cast<ppb::wire_type>(tag % 8)`).  When several
  error-semantics fields hit on the same scan, which one is reported
  is unspecified.  An `unknown<W, field_semantics::error>` catch-all
  reports a sentinel `0xFFFFFFF8u | uint32_t(W)` (it has no real
  field number); decoded with `tag / 8` it collides with field
  `2**29 - 1`.
- `unknown_field()`: `optional<uint64_t>` set when a catch-all
  registered via `ppb::unknown<wire>` saw at least one occurrence.
  Same encoding as `error_field()`.  Which occurrence is reported
  (across multiple unknown tags within one wire bucket, or across
  the four wire-type catch-alls) is unspecified.  Sticky and
  **soft**: once set, later calls leave it alone, and it does *not*
  set the reader's error or abort dispatch.  Populated by `parse()`
  and `prescan()`; standalone `lexn()` leaves it alone.

### Resetting between messages

For a stream of independent messages, build a fresh reader per
message:

```cpp
ppb::reader<Schema> r(first_message);
for (;;) {
    if (auto err = r.parse(init, handlers...); err != PPB_OK)
        break;
    // ... use the decoded values ...
    r = ppb::reader<Schema>(next_message);
}
```

For sub-message streams that don't need an outer prescan, `lex_all()`
replaces the manual `while (!r.empty()) r.lexn(...)` loop.

`reset_fields()` is a narrower tool: it zeroes per-field metadata for
another `prescan` or `parse` call (e.g., to keep consuming from the
input span). It does **not** clear the input span, `error()`,
`error_field()`, or `unknown_field()`, so a reader that has already
failed cannot be revived this way.

Limits
------

`ppb::limit` bundles byte and field caps (hard or soft) and a
recursion-depth budget for `on_submessage`:

```cpp
ppb::limit::max_fields(n)        // cap toplevel field count
ppb::limit::max_depth(n)         // sub-message recursion budget (default 0)
ppb::limit::hard(bytes)          // PPB_ERROR_LIMIT_EXCEEDED past `bytes`
ppb::limit::soft(bytes)          // stop near `bytes`, no error
ppb::limit::hard(bytes, fields)  // hard byte cap plus field cap
ppb::limit::soft(bytes, fields)  // soft byte cap plus field cap
```

`with_max_fields`, `with_max_depth`, `with_hard_limit`,
`with_soft_limit` are chainable builders; `with_max_bytes` is also
chainable, and changes only the byte limit without affecting the
hard/soft nature of the limit.

`ppb::on_submessage<K, InnerSchema>` requires
`limit::max_depth(N)` with N > 0; the default 0 fails closed with
`PPB_ERROR_DEPTH_EXCEEDED`.

Limitations
-----------

- GCC/Clang only.  The code (C and C++) uses GNU extensions; MSVC is
  not supported.
- No bounds checking on `enumerated`: wire values that don't name any
  enumerator reach the handler as the corresponding bit pattern.

Compile-time diagnostics
------------------------

The header relies on `static_assert`.  Common messages:

| Trigger                          | Message fragment                                           |
|----------------------------------|------------------------------------------------------------|
| `varint<0>`, `varint<1<<29>`     | `field tag key must be convertible to uint64_t and must fit in [1, 2**29 - 1].` |
| `field_base<K, wire_type(3)>`    | `field wire type must be one of varint, i64, len, or i32`  |
| `unknown<wire_type::any>`        | `ppb::unknown wire type must be one of varint, i64, len, or i32` |
| `schema<>`                       | `schema must include at least one field`                   |
| `schema<int>`                    | `schema template arguments must be field_generic_base`     |
| `schema<varint<1>, varint<1L>>`  | `schema fields must all have the same Key type`            |
| `schema<varint<2>, i64<1>>`      | `schema fields must be listed in strictly ascending order` |
| `auto_schema<varint<1>, int>`    | `auto_schema arguments must be field_generic_base (possibly nested in std::tuple)` |
| `r.meta<absent>()`               | `Key/wire combination not found in schema`                 |
| `bytes<K, Misaligned>`           | `bytes element type must be byte-aligned`                  |
| Handler key type mismatch        | `every value_handler in the tuple must use the same key type as the dispatch Key` |
| Handlers, none invocable         | `value_handlers match (Key, wire), but none is invocable with the argument type` |
| Handlers, multiple invocable     | `multiple value_handlers match (Key, wire) and accept the argument type; ambiguous` |
| `on_unknown`, none invocable     | `unknown_handlers match the wire type, but none is invocable with the argument type` |
| `on_unknown`, multiple invocable | `multiple unknown_handlers match the wire type and accept the argument type; ambiguous` |
| Orphan handler                   | `on<Key> or on_unknown<> handler does not match any schema field` |
| Schema mismatch on `on_submessage` | `ppb::message<K, S> field paired with ppb::on_submessage<K, S2> requires S == S2` |

Matching positive and negative tests live in
`tests/test_ppb_cpp_static.cc` and `tests/test_ppb_cpp_compile_fail.cc`.
