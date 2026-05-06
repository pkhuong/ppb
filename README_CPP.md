`ppb::reader`: C++ wrapper for PPB
===================================

`<ppb/ppb.hpp>` is a header-only C++20 wrapper around the C lexer in
`<ppb/ppb.h>`.  It compiles a typed schema down to the same
encoded-tag arrays the C library consumes, then dispatches matched
fields to caller-supplied handlers.  Like the C library, it never
allocates and instead borrows the encoded input bytes: the caller
passes in a byte span and receives values that point back into that
span.  The C library is formally verified and fuzzed.  The C++ wrapper
is merely unit tested.

You'll want to understand the C API before using this convenience
header.  For the underlying lexer's contract (prescan-then-lex,
limits, error codes) see the comments in `ppb.h` and the top-level
`README.md`.  The C++ wrapper doesn't use `ppb_validate_tags`: the
function is out of line, so not constexpr-friendly, but
`ppb::schema<>` performs the same checks at compile-time.

**Important note for schemas with a large number of fields** while the
C library guarantees Θ(n log m) time, where n is the number of
toplevel fields consumed and m is the number of fields, dispatching to
a field handler in this C++ interface *may* compile to a linear-time
traversal.  The worst case is thus Θ(m · n) time, but that requires
pessimal compilation, and comes with a pretty low constant factor.

Quick start
-----------

```cpp
#include <ppb/ppb.hpp>

enum class MyMessage : int {
    count   = 1,
    name    = 2,
    payload = 3,
};

using Schema = ppb::schema<
    ppb::int32 <MyMessage::count>,
    ppb::utf8string<MyMessage::name>,
    ppb::bytes <MyMessage::payload>>;

ppb_error parse_message(std::span<const std::byte> bytes)
{
    int32_t count = 0;
    std::string_view name;
    std::span<const std::byte> payload;

    ppb::reader<Schema> r(bytes);

    return r.parse(
        [&](const ppb::reader<Schema> &snap) -> ppb_error {
            // Optional: inspect snap.meta<MyMessage::name>() to preallocate.
            return PPB_OK;
        },
        ppb::limit{},  // no special limit
        ppb::on<MyMessage::count>  ([&](int32_t v)                   { count   = v; return PPB_OK; }),
        ppb::on<MyMessage::name>   ([&](std::string_view v)          { name    = v; return PPB_OK; }),
        ppb::on<MyMessage::payload>([&](std::span<const std::byte> v){ payload = v; return PPB_OK; }));
}
```

Schema and field types
----------------------

A schema is a type list of field descriptors:

```cpp
using S = ppb::schema<ppb::varint<1>, ppb::len<2>, ppb::i32<3>>;
```

Constraints (all enforced by `static_assert`):
- At least one field.
- Every field's `Key` type must match (e.g. all plain `int`, or all the
  same `enum class`).
- Field tags must be strictly ascending in encoded-tag order.  When the
  same field number appears with multiple wire types, list them in
  `enum ppb_wire_type` order (varint, i64, len, i32).

Wire-typed primitives
---------------------

`ppb::varint<K>`, `ppb::i64<K>`, `ppb::len<K>`, `ppb::i32<K>` are the
"raw" field types.  Handlers for these receive `const ppb_field &`.

### Typed scalars

These wrap a wire-typed primitive and decode into a native C++ type, so
handlers receive typed argument:

| Field type                    | Handler argument         |   Wire type |
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

`enumerated<K, Enum>` requires `Enum` to have a fixed underlying type.
Wire values are first cast to the underlying type and then to `Enum`,
which is well-defined for any 64-bit input.  No range checking is done;
out-of-range values reach the handler as the corresponding bit pattern
in `Enum`'s underlying type.

`bytes<K, Element>` requires `alignof(Element) == 1`.  If the payload's
length is not a multiple of `sizeof(Element)`, the reader's error is set
to `PPB_ERROR_TRUNCATED_DATA` and the handler is invoked with an empty
span.

### Packed and unpacked repeated fields

For `packed`-encoded scalars the wrapper provides:

- Fixed-width: `packed_fixed32`, `packed_sfixed32`, `packed_f32`,
  `packed_fixed64`, `packed_sfixed64`, `packed_f64`.  Handlers receive
  a `std::span<const ppb::le_packed<T>>`.  `le_packed<T>` is a packed,
  unaligned, little-endian wrapper; convert to host order with
  `static_cast<T>(elem)` or `elem.value()`.
- Varint: `packed_int32`, `packed_int64`, `packed_sint32`,
  `packed_sint64`, `packed_uint32`, `packed_uint64`, `packed_boolean`,
  `packed_enumerated<K, Enum>`.  Handlers receive a lazy view with an
  iterator that decodes one varint per `++`.  Decode failures set the
  reader's error (`PPB_ERROR_TRUNCATED_DATA` or `PPB_ERROR_CORRUPT_VARINT`)
  and exhaust the iterator.

`unpacked_<scalar><K>` aliases (`unpacked_int32`, etc.) are convenience
typedefs for the corresponding `<scalar><K, field_semantics::repeated>`.

### Field semantics

Each scalar field accepts an optional `field_semantics` template
parameter that controls how `parse()` dispatches when the same field
appears multiple times on the wire:

| `field_semantics`     | Behavior |
|-----------------------|----------|
| `repeated`            | Force per-occurrence dispatch when there are multiple occurrences. |
| `singular`            | Last-write-wins, *but* upgrades to per-occurrence when the C lexer reports `lost_distinct_u64` (varint/i32/i64 with distinct values).  **For LEN fields, repeats always force per-occurrence dispatch**: PPB does not compare LEN payload contents, so distinct values cannot be detected, and dropping any of them silently would be wrong. |
| `last_write_wins`     | Truly LWW: only the last occurrence is dispatched, even for distinct LEN payloads. Matches proto3 default semantics. (default for typed scalars) |
| `always_lexn`         | Force per-occurrence dispatch even on a single occurrence; useful when handler order must follow wire order (default for raw `varint`/`i64`/`len`/`i32`). |
| `error`               | Treat any occurrence as a parse error (see "Error reporting" below). |

Packed fields are always `repeated`.

Reading a message
-----------------

```cpp
ppb::reader<Schema> r(bytes);  // or r(ptr, length)
```

The reader holds a byte span, a sticky error info, and an array of
per-field state.  All of this is trivial / no-allocation; copies and
moves are cheap (relatively... still a large `memcpy` [64 bytes per
field] if you have tens of fields).

### `parse()`: the high-level entry point

```cpp
ppb_error parse(Init init, ppb::limit bounds = {}, Handlers... handlers);
```

1. Runs `ppb_prescan` over the input subject to `bounds`.
2. Checks `field_semantics::error` fields and decides whether the lexn
   pass is needed.
3. Calls `init(std::as_const(*this))`; the snapshot lets you call
   `meta<Key>()` to size buffers.  If `init` returns non-`PPB_OK`, the
   error is recorded and the input span is **not** consumed.
4. If no field forces lexn: dispatches handlers once with the
   prescan-aggregated values, then advances the input span past the
   prescanned bytes.
5. Otherwise: runs `ppb_lexn` in a loop, dispatching handlers per
   batch; advances the input span as bytes are consumed.

If `prescan` reports zero bytes (e.g. empty input or `max_fields(0)`),
`parse()` returns `PPB_OK` immediately *without* invoking `init` or
any handlers.

Handler errors are sticky on the reader (first non-`PPB_OK` wins) but
all handlers in a given batch are still invoked (so as to match the
bytes consumed by prescan or lexn).

### Lower-level entry points

- `prescan(limit, handlers...)`: runs `ppb_prescan` and dispatches
  matching handlers once with the aggregated values.  Returns
  `ptrdiff_t` bytes consumed (>= 0) or a negative `ppb_error`.  Does
  *not* advance the input span.
- `lexn(limit, handlers...)`: runs one `ppb_lexn` batch and dispatches
  handlers for that batch.  Advances the input span.  Call in a loop
  until the input is empty or an error is set.
- `dispatch(handlers...)`: re-dispatches handlers against the current
  field state without re-reading from the wire.  Mostly useful for
  tests.

### `meta<Key, wire>()`

Returns the `ppb_field_meta` aggregated by the most recent prescan.
When no explicit wire type is provided, results from all schema
entries that match `Key` are merged.  Errors at compile time when no
schema entry matches.

Handlers and `ppb::on<>()`
--------------------------

```cpp
ppb::on<Key, wire = wire_type::any>(callable)
```

- `Key` must have the same C++ type as the schema's `Key`. Mixing
  `enum class FieldId` with raw integer literals will produce a
  `static_assert` failure ("every value_handler in the tuple must use
  the same key type as the dispatch Key").  Either consistently use
  the enum, or consistently use plain integers.
- When multiple handlers match the same `(Key, wire)` pair, the wrapper
  selects the one whose argument type matches the field's
  `extract_value` return.  If zero or more than one survives, the
  ambiguity is reported at compile time.
- Handlers must return `ppb_error`.

Error reporting
---------------

The reader carries a single sticky `ppb_error`:

- `error()` returns the current error.  Once set, `prescan` / `lexn` /
  `parse` short-circuit and return that error without further work.
- `error_field()` returns an `optional<uint32_t>` set when a
  `field_semantics::error` field appeared on the wire.  The value is
  the encoded tag, i.e. `field_number * 8 + wire_type`.

### Resetting between messages

`reset_fields()` zeroes the per-field metadata so the same reader can
be aimed at the next message.  It does **not** clear `error()` or
`error_field()`: the sticky-error contract is intentional, and a
reader that has already errored cannot be revived.  To process a
stream of messages where some may fail, construct a fresh `reader`
per message (or wrap the loop so a failed reader is replaced).

The intended success-path pattern is:

```cpp
ppb::reader<Schema> r(first_message);
for (;;) {
    if (auto err = r.parse(init, {}, handlers...); err != PPB_OK)
        break;
    // ... use the decoded values ...
    r = ppb::reader<Schema>(next_message);  // or r.reset_fields() if reusing the span
}
```

Limits
------

`ppb::limit` mirrors the C library's `(max_fields, byte_limit, limit_error)`
triple:

```cpp
ppb::limit::max_fields(n)        // cap number of fields
ppb::limit::hard(bytes)          // PPB_ERROR_LIMIT_EXCEEDED past `bytes`
ppb::limit::soft(bytes)          // stop near `bytes`, no error
ppb::limit::hard(bytes, fields)  // both
```

`with_max_fields`, `with_hard_limit`, `with_soft_limit` are chainable
builders.

Limitations / known gaps
-----------------------

- **No catch-all support.** The C library's `PPB_TAG(-1, wire)` matches
  any unknown field of a wire type, but the C++ schema's tag-range
  static-assert excludes negative / `UINT64_MAX` keys.  Use the C API
  directly if you need catch-all dispatch.
- **GCC/Clang only.** `le_packed<T>` uses `[[gnu::packed]]`, so MSVC
  is not currently supported.
- **No bounds checking on `enumerated`.** Wire values that don't name
  any enumerator reach the handler unchanged.

Compile-time diagnostics
------------------------

The header makes heavy use of `static_assert`; the more common errors
and their messages:

| Trigger                          | Message fragment                                           |
|----------------------------------|------------------------------------------------------------|
| `varint<0>`, `varint<1<<29>`     | `field tag key must be convertible to uint64_t`            |
| `field_base<K, wire_type(3)>`    | `field wire type must be one of varint, i64, len, or i32`  |
| `schema<>`                       | `schema must include at least one field`                   |
| `schema<int>                   ` | `schema template arguments must be field_generic_base`     |
| `schema<varint<1>, varint<1L>>`  | `schema fields must all have the same Key type`            |
| `schema<varint<2>, i64<1>>`      | `schema fields must be listed in strictly ascending order` |
| `r.meta<absent>()`               | `Key not found in schema`                                  |
| Handler key type mismatch        | `every value_handler in the tuple must use the same key type as the dispatch Key` |
| Two handlers, none invocable     | `value_handlers match (Key, wire), but none is invocable with the argument type` |
| Two handlers, both invocable     | `multiple value_handlers match (Key, wire) and accept the argument type; ambiguous` |

Matching positive and negative tests live in
`tests/test_ppb_cpp_static.cc` and `tests/test_ppb_cpp_compile_fail.cc`.
