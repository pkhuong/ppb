#pragma once

#include "ppb.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

// PPB C++ wrapper: a header-only, allocation-free, type-safe facade
// over the C lexer in <ppb/ppb.h>.
//
// The wrapper compiles a typed `ppb::schema<...>` down to the
// encoded-tag arrays the C library consumes, then dispatches matched
// fields to caller-supplied `ppb::on<Key>(callable)` handlers with
// natural C++ argument types.
//
// Like the C library, the wrapper never allocates and instead borrows
// the encoded input bytes: callers pass in a byte span
// (`std::span<const std::byte>`) and decoded values either point back
// into that span (string_view, span<const Element>, lazy varint
// views) or are returned by value (scalars).  Lifetimes are the
// caller's responsibility: pretty much anything passed to handlers
// borrows from the input span.
//
// The C++ library never calls `ppb_validate_tags`: `ppb::schema<>`
// instead performs the same validation at compile-time.
//
// Public API surface:
//   - `ppb::schema<Fs...>`: type-list of field descriptors, validated
//     at compile time (non-empty, single Key type, strictly ascending
//     encoded tags, in-range field numbers).
//   - `ppb::reader<Schema>`: stateful driver around a schema and an
//     input span; owns the per-field state buffer.
//   - `reader::parse(...)`: prescan + optional lexn-loop + handler
//     dispatch.  Accepts `parse(handlers...)`,
//     `parse(limit, handlers...)`, `parse(init, handlers...)`,
//     `parse(limit, init, handlers...)`, and the canonical
//     `parse(init, limit, handlers...)`.  Use `init` to inspect
//     prescan metadata and preallocate.  Lower-level entry points
//     (`prescan`, `lexn`, `dispatch`) are also available.
//   - `ppb::on<Key, wire = any>(callable)`: handler factory.
//     Multiple handlers can share a key; the wrapper picks the one
//     whose argument type matches the matched field, and reports
//     ambiguity at compile time.
//   - `ppb::limit`: byte / field caps, hard or soft.
//
// Field descriptors:
//   - Wire-typed primitives: `varint`, `i64`, `len`, `i32`.  Handlers
//     receive `const ppb_field &`.
//   - Typed scalars (varint-backed): `int32`, `int64`, `sint32`,
//     `sint64`, `uint32`, `uint64`, `boolean`, `enumerated<K, Enum>`.
//   - Typed scalars (i32/i64-backed): `fixed32`, `sfixed32`, `f32`,
//     `fixed64`, `sfixed64`, `f64`.
//   - LEN-backed: `utf8string` (-> `std::string_view`),
//     `bytes<K, Element=std::byte>` (-> `std::span<const Element>`).
//   - Packed repeated: `packed_fixed32`/`sfixed32`/`f32`/`fixed64`/
//     `sfixed64`/`f64` (-> span of `le_packed<T>`),
//     `packed_int32`/`int64`/`sint32`/`sint64`/`uint32`/`uint64`/
//     `boolean`/`enumerated` (-> lazy varint view).
//   - Unpacked repeated aliases: `unpacked_<scalar>` =
//     `<scalar><K, field_semantics::repeated>`.
//
// Field semantics (`enum class field_semantics`, see the enum's own
// comment for full details) control how `parse()` dispatches when the
// same field appears multiple times: `repeated` and `always_lexn`
// force per-occurrence dispatch; `last_write_wins` always squashes;
// `singular` squashes when the C lexer reports the squash was
// lossless (varint/i32/i64 with bit-identical values), but always
// forces lexn for LEN since the C lexer does not compare LEN
// payloads; `proto3_zero_default` is like `last_write_wins` but *may*
// also dispatch absent fields (handler receives zero u64 or empty
// span); `error` rejects any occurrence with PPB_ERROR_CORRUPT_TAG.
//
// Error handling:
//   - `reader::error()` is a sticky `ppb_error`: once set, every
//     subsequent `prescan`/`lexn`/`parse`/`dispatch` short-circuits.
//     `reset_fields()` does *not* clear it; construct a fresh reader
//     to recover.
//   - `reader::error_field()` reports the encoded tag of any
//     `field_semantics::error` field that triggered, as
//     `field_number * 8 + wire_type`.
//   - Handler errors are accumulated first-write-wins; every handler
//     in the current lexn batch still runs after a sibling errors.
//   - Soft decoding errors inside `extract_value` (e.g. misaligned
//     `bytes<K, Element>` payload, mid-payload truncated packed
//     varint) set the reader's error and hand the handler a partial
//     result (an empty span, a view that exhausts at the failure).
//
// Compile-time diagnostics:
//   The header makes heavy use of `static_assert`. Field-tag range,
//   wire type, schema ordering / non-emptiness / single-Key-type, and
//   handler key-type / arg-type matching are all caught at compile
//   time.  `tests/test_ppb_cpp_compile_fail.cc` enumerates the
//   diagnostics; `tests/test_ppb_cpp_static.cc` covers the matching
//   positive cases.
//
// Limitations:
//   - Field numbers are restricted to [1, 2**29), same as protobuf.
//     Detect unknown tags with dedicated `ppb::unknown<wire>` (or
//     `ppb::detect_unknown_fields` to register all four wire types at
//     once); `reader::unknown_field()` then reports the encoded tag
//     (`field_number * 8 + wire_type`) of the first unknown seen.
//     This detection runs after prescan (directly, or via reader::parse).
//   - `enumerated<K, Enum>` does not validate that wire values name
//     a declared enumerator: the cast goes through `Enum`'s underlying
//     type so it is always well-defined, but out-of-range values may
//     reach the handler.

namespace ppb
{

// Protobuf wire types, matching `enum ppb_wire_type`.
//
// `any` is a sentinel used by `reader::meta<>()` and `ppb::on<>()` to
// mean "match all wire types associated with this key".  It is not a
// valid wire type for a field descriptor; `field_base` static-asserts
// against it.
enum class wire_type : uint8_t
{
    varint = PPB_WIRE_VARINT,
    i64 = PPB_WIRE_I64,
    len = PPB_WIRE_LEN,
    i32 = PPB_WIRE_I32,

    any = 255,  // sentinel value for meta<>
};

// Per-field policy used by `reader::parse()` to decide how to
// dispatch when the same field appears multiple times on the wire.
// All semantics are advisory. Handler dispatch is the only behavior
// that changes; prescan metadata is always aggregated the same way.
// Handlers may be called up to once for every occurrence on the wire,
// and all handlers are skipped on empty inputs.
//
//   always_lexn     Force a lexn pass even on a single occurrence, so
//                   handlers are invoked in wire order.  Default for
//                   the raw `varint` / `i64` / `len` / `i32` types.
//
//   repeated        We want to see every occurrence.  A single
//                   occurrence is dispatched once from the prescan
//                   aggregate (no extra lexn); two or more occurrences
//                   force a lexn pass.
//
//   singular        Last-write-wins is acceptable *iff* the squashed
//                   values are bit-identical (the C lexer reports this
//                   via `lost_distinct_u64`).  Otherwise we force a
//                   lexn pass so the handler sees each occurrence.
//
//                   N.B.: for LEN fields, `lost_distinct_u64` is never
//                   set (the C lexer does not compare LEN payloads),
//                   so any LEN field with two or more occurrences
//                   forces a lexn pass.  Use `last_write_wins` if you
//                   just want to drop earlier LEN payloads.
//
//   last_write_wins Only the last occurrence is dispatched, regardless
//                   of wire type.  Matches proto3 default field
//                   semantics for scalars.
//
//   proto3_zero_default Like `last_write_wins` (never forces a lexn
//                   pass) but the handler *may* be invoked even when
//                   the field was absent from the wire, in which case
//                   the handler receives the zero-filled default (0
//                   for scalars, empty span for LEN).
//
//   error           Any occurrence is a parse error: parse() returns
//                   PPB_ERROR_CORRUPT_TAG, sets `reader::error_field()`
//                   to the encoded tag (field_number * 8 + wire_type),
//                   and does not invoke `init` or any handlers.
enum class field_semantics : int8_t
{
    always_lexn = -1,  // always force lexn, even for a single occurrence
    repeated = 0,  // we want to see everything
    singular = 1,  // it's ok to do LWW if all the squashed values are equivalent
    last_write_wins = 2,  // we only want to see the last value (regular proto semantics)
    proto3_zero_default = 3,  // LWW but also dispatch absent fields as zero/empty
    error = 127,  // just report a PPB_ERROR_CORRUPT_TAG if we see this
};

// PPB field descriptors all inherit from `field_base<K, T>`, which is-a
// `field_generic_base`.
struct field_generic_base;
template <auto K, wire_type type> struct field_base;

// A varint-encoded field
template <auto K, field_semantics sem = field_semantics::always_lexn> struct varint;
// An i64-encoded field
template <auto K, field_semantics sem = field_semantics::always_lexn> struct i64;
// A length-prefixed field
template <auto K, field_semantics sem = field_semantics::always_lexn> struct len;
// An i32-encoded field
template <auto K, field_semantics sem = field_semantics::always_lexn> struct i32;

// Catch-all descriptor for the C lexer's `PPB_TAG(-1, wire)` entry.
// Drop one (or all four, via `detect_unknown_fields`) into a schema
// to register the catch-all so unknown tags get accounted for instead
// of silently skipped, and so `reader::unknown_field()` can report
// when one was seen.  No need for a `Key` type: these work the same
// with any schema Key type.
template <wire_type type, field_semantics sem = field_semantics::repeated> struct unknown;

// Varint-backed scalar field types (proto2 last-write-wins semantics by default)
template <auto K, field_semantics sem = field_semantics::last_write_wins> struct int32;
template <auto K, field_semantics sem = field_semantics::last_write_wins> struct int64;
template <auto K, field_semantics sem = field_semantics::last_write_wins> struct sint32;
template <auto K, field_semantics sem = field_semantics::last_write_wins> struct sint64;
template <auto K, field_semantics sem = field_semantics::last_write_wins> struct uint32;
template <auto K, field_semantics sem = field_semantics::last_write_wins> struct uint64;
template <auto K, field_semantics sem = field_semantics::last_write_wins> struct boolean;

namespace detail
{

template <typename T>
concept enum_type = std::is_enum_v<T>;

template <typename T, bool = enum_type<T>> struct enum_underlying_type_impl
{
    using type = void;
};

template <typename T> struct enum_underlying_type_impl<T, true>
{
    using type = std::underlying_type_t<T>;
};

template <typename T> using enum_underlying_type = typename enum_underlying_type_impl<T>::type;

}  // namespace detail

// Varint-backed enum field.  `Enum` must have a fixed underlying type
// (e.g. `enum class Color : uint32_t`).  Wire values are first
// converted to `Enum`'s underlying type and then to `Enum`, which is
// well-defined for any 64-bit input.  No range checking is performed:
// values that don't name an enumerator reach the handler as the
// corresponding bit pattern in the underlying type.
//
// proto2 last-write-wins semantics by default.
template <auto K, typename Enum, field_semantics sem = field_semantics::last_write_wins,
    typename UnderlyingType = detail::enum_underlying_type<Enum>>
    requires detail::enum_type<Enum>
struct enumerated;

// I32-backed scalar field types. proto2 last-write-wins semantics by default
template <auto K, field_semantics sem = field_semantics::last_write_wins> struct fixed32;
template <auto K, field_semantics sem = field_semantics::last_write_wins> struct sfixed32;
template <auto K, field_semantics sem = field_semantics::last_write_wins> struct f32;

// I64-backed scalar field types. proto2 last-write-wins semantics by default
template <auto K, field_semantics sem = field_semantics::last_write_wins> struct fixed64;
template <auto K, field_semantics sem = field_semantics::last_write_wins> struct sfixed64;
template <auto K, field_semantics sem = field_semantics::last_write_wins> struct f64;

// LEN-backed scalar field types. proto2 last-write-wins semantics by default
template <auto K, field_semantics sem = field_semantics::last_write_wins> struct utf8string;
template <auto K, typename Element = std::byte, field_semantics sem = field_semantics::last_write_wins>
struct bytes;

// Sub-message field: a LEN payload that the C++ wrapper knows is itself
// a protobuf-encoded message described by `InnerSchema` (a
// `ppb::schema<...>`).  Inherits from `bytes<K, std::byte, sem>`, so on
// the wire and during prescan/lexn it behaves identically to a plain
// `bytes` field; the additional `inner_schema` typedef lets
// `ppb::on_submessage<K, S>` perform a compile-time cross-check that
// the schema author and the handler agree on the inner type.
//
// Defaults to `last_write_wins` (not `singular`): repeat occurrences of
// the same key are merged proto-style by overwrite, with no bit-identity
// check on the encoded payload.
template <auto K, typename InnerSchema, field_semantics sem = field_semantics::last_write_wins>
struct message;

// Convenience aliases that hardcode proto3_zero_default semantics.  Users musn't
// rely on the behavior being any different than last_write_wins, but it can be a
// little more efficient to unconditionally handle a zero-filled value.
template <auto K> using proto3_int32 = int32<K, field_semantics::proto3_zero_default>;
template <auto K> using proto3_int64 = int64<K, field_semantics::proto3_zero_default>;
template <auto K> using proto3_sint32 = sint32<K, field_semantics::proto3_zero_default>;
template <auto K> using proto3_sint64 = sint64<K, field_semantics::proto3_zero_default>;
template <auto K> using proto3_uint32 = uint32<K, field_semantics::proto3_zero_default>;
template <auto K> using proto3_uint64 = uint64<K, field_semantics::proto3_zero_default>;
template <auto K> using proto3_boolean = boolean<K, field_semantics::proto3_zero_default>;

template <auto K, typename Enum>
using proto3_enumerated = enumerated<K, Enum, field_semantics::proto3_zero_default>;

template <auto K> using proto3_fixed32 = fixed32<K, field_semantics::proto3_zero_default>;
template <auto K> using proto3_sfixed32 = sfixed32<K, field_semantics::proto3_zero_default>;
template <auto K> using proto3_f32 = f32<K, field_semantics::proto3_zero_default>;

template <auto K> using proto3_fixed64 = fixed64<K, field_semantics::proto3_zero_default>;
template <auto K> using proto3_sfixed64 = sfixed64<K, field_semantics::proto3_zero_default>;
template <auto K> using proto3_f64 = f64<K, field_semantics::proto3_zero_default>;

template <auto K> using proto3_utf8string = utf8string<K, field_semantics::proto3_zero_default>;
template <auto K, typename Element = std::byte>
using proto3_bytes = bytes<K, Element, field_semantics::proto3_zero_default>;
// No proto3_message alias: proto3 message fields behave identically
// to proto2 message fields.  Optional fields have explicit presence,
// like proto2 last_write_wins (in fact, early proto3 recommended
// wrapping scalars in submessages specifically to detect presence)
// Use plain message<K, InnerSchema> for both proto2 and proto3.

// Wraps a fixed-width value stored as little-endian, unaligned bytes
// (the protobuf wire format for fixed32/sfixed32/fixed64/sfixed64/
// float/double).  Conversion to `T` returns the host-order value,
// byte-swapping if the host is big-endian.
//
// The type is `[[gnu::packed]]` so that `alignof(le_packed<T>) == 1`,
// which lets `bytes<K, le_packed<T>>` reinterpret a LEN payload as a
// span of `le_packed<T>` regardless of payload alignment.
template <typename T> struct [[gnu::packed]] le_packed
{
    static_assert(std::is_trivially_copyable_v<T>,
        "ppb::le_packed requires a trivially copyable element type");
    static_assert(sizeof(T) == 4 || sizeof(T) == 8, "ppb::le_packed only supports 4- or 8-byte values");

    T value() const;
    [[gnu::always_inline]] operator T() const { return value(); }

private:
    T x;
};

// Packed repeated field types
template <auto K> struct packed_int32;
template <auto K> struct packed_int64;
template <auto K> struct packed_sint32;
template <auto K> struct packed_sint64;
template <auto K> struct packed_uint32;
template <auto K> struct packed_uint64;
template <auto K> struct packed_boolean;
template <auto K, typename Enum, typename UnderlyingType = detail::enum_underlying_type<Enum>>
    requires detail::enum_type<Enum>
struct packed_enumerated;
template <auto K> struct packed_fixed32;
template <auto K> struct packed_sfixed32;
template <auto K> struct packed_f32;
template <auto K> struct packed_fixed64;
template <auto K> struct packed_sfixed64;
template <auto K> struct packed_f64;

// Unpacked repeated field types
template <auto K> using unpacked_int32 = int32<K, field_semantics::repeated>;
template <auto K> using unpacked_int64 = int64<K, field_semantics::repeated>;
template <auto K> using unpacked_sint32 = sint32<K, field_semantics::repeated>;
template <auto K> using unpacked_sint64 = sint64<K, field_semantics::repeated>;
template <auto K> using unpacked_uint32 = uint32<K, field_semantics::repeated>;
template <auto K> using unpacked_uint64 = uint64<K, field_semantics::repeated>;
template <auto K> using unpacked_boolean = boolean<K, field_semantics::repeated>;
template <auto K, typename E> using unpacked_enumerated = enumerated<K, E, field_semantics::repeated>;
template <auto K> using unpacked_fixed32 = fixed32<K, field_semantics::repeated>;
template <auto K> using unpacked_sfixed32 = sfixed32<K, field_semantics::repeated>;
template <auto K> using unpacked_f32 = f32<K, field_semantics::repeated>;
template <auto K> using unpacked_fixed64 = fixed64<K, field_semantics::repeated>;
template <auto K> using unpacked_sfixed64 = sfixed64<K, field_semantics::repeated>;
template <auto K> using unpacked_f64 = f64<K, field_semantics::repeated>;
template <auto K> using unpacked_utf8string = utf8string<K, field_semantics::repeated>;
template <auto K, typename Element = std::byte>
using unpacked_bytes = bytes<K, Element, field_semantics::repeated>;
template <auto K, typename InnerSchema>
using unpacked_message = message<K, InnerSchema, field_semantics::repeated>;

// Convenience tuple that registers the catch-all entry for every wire
// type at once.  Use with `auto_schema` to opt into unknown-field
// detection without listing the four unknown fields explicitly:
//
//   using S = ppb::auto_schema<MyFields..., ppb::detect_unknown_fields>;
//
// Detection happens in `reader::prescan` or `reader::parse`.
using detect_unknown_fields = std::tuple<unknown<wire_type::varint>, unknown<wire_type::i64>,
    unknown<wire_type::len>, unknown<wire_type::i32>>;

}  // namespace ppb

// clang-format off
#include "ppb_detail.hpp"
// clang-format on

namespace ppb
{

// Compile-time schema: a strictly-ascending type list of field descriptors.
//
// All fields must share the same `Key` type (an `enum class`), except
// for unknown fields, which have no key at all.  Tags must be in
// strictly ascending encoded order; when the same field number
// appears with multiple wire types, list them in `enum ppb_wire_type`
// order (varint, i64, len, i32).
//
// `field<I>()` returns a default-constructed instance of the I-th
// field type; use `decltype(schema::template field<I>())` to recover
// the type itself.  `s_encoded_tags` is the parallel array of
// `ppb_encoded_tag`s that the C lexer consumes.
template <typename... Fs> struct schema : private detail::schema_impl<Fs...>
{
private:
    using impl = detail::schema_impl<Fs...>;

public:
    using Key = typename impl::Key;

    static_assert(impl::fields_non_empty(), "schema must include at least one field");
    static_assert(impl::fields_are_fields(), "schema template arguments must be field_generic_base");
    static_assert(impl::fields_have_same_key_type(), "schema fields must all have the same Key type");
    static_assert(impl::tags_are_in_order(), "schema fields must be listed in strictly ascending order");
    static_assert(sizeof(size_t) < sizeof(uint32_t) || sizeof...(Fs) <= (size_t(1) << 31),
        "we must have at most 2**31 fields");

    static constexpr size_t num_fields() { return sizeof...(Fs); }

    // Returns a default-constructed instance of the field type at
    // position `I` in the schema. Use `decltype(...)` on the result
    // to recover the type itself.
    template <size_t I> static consteval auto field()
    {
        static_assert(I < num_fields(), "field index must be less than num_fields()");
        if constexpr (I < num_fields())
        {
            return std::tuple_element_t<I, std::tuple<Fs...>> {};
        }
    }

    static constexpr std::array<ppb_encoded_tag, num_fields()> s_encoded_tags = impl::encoded_tags();

    // implementation details for reader<>.
    using impl::find_field_range;
    using typename impl::field_range;
};

// Adapter that accepts a list of field types and/or (possibly nested)
// `std::tuple<...>` collections of fields, flattens them into a
// single type list, validates that every leaf is a `field_generic_base`
// subclass, sorts them ascending by `(field number, wire type)`, and
// yields the corresponding `ppb::schema<...>`.
//
// Use this when assembling a schema from reusable sub-tuples: callers
// no longer need to manually keep field declarations in tag order.
//
//   using shared = std::tuple<ppb::varint<3>, ppb::len<7>>;
//   using my_schema = ppb::auto_schema<ppb::varint<1>, shared, ppb::i32<2>>;
//   // == ppb::schema<ppb::varint<1>, ppb::i32<2>, ppb::varint<3>, ppb::len<7>>

// Duplicate `(key, wire)` pairs are rejected by `schema<>`'s
// strictly-ascending check.
template <typename... Ts> using auto_schema = typename detail::sorted_fields<Ts...>::template to<schema>;

// Bundles the `(max_fields, max_bytes, error_on_bytes)` triple for
// `ppb_prescan_*` / `ppb_lexn_*`.
//
// Construct with one of the named factories:
//   limit::max_fields(n)         cap toplevel-field count
//   limit::hard(bytes [, n])     stop at or after `bytes` and error if exceeded
//   limit::soft(bytes [, n])     stop at or after `bytes`, no error if exceeded
//
// The `with_*` builders chain additional caps onto an existing limit.
// Note that `with_max_bytes` leaves the byte-limit-error code alone, so
// chaining `limit::max_fields(n).with_max_bytes(b)` produces a *soft*
// byte limit; for a hard one, use `with_hard_limit` (or the `hard`
// factory).
struct limit
{
public:
    static constexpr limit max_fields(size_t max) noexcept { return limit().with_max_fields(max); }

    // Maximum recursion depth for `on_submessage` auto-traversal.
    // Default 0 means submessage traversal is rejected: callers must
    // opt in by setting this to a positive value.  See
    // `ppb::on_submessage` and `PPB_ERROR_DEPTH_EXCEEDED`.
    static constexpr limit max_depth(uint32_t max) noexcept { return limit().with_max_depth(max); }

    static constexpr limit hard(size_t max_bytes,
        size_t max_fields = std::numeric_limits<size_t>::max()) noexcept
    {
        return limit().with_hard_limit(max_bytes).with_max_fields(max_fields);
    }

    static constexpr limit soft(size_t max_bytes,
        size_t max_fields = std::numeric_limits<size_t>::max()) noexcept
    {
        return limit().with_soft_limit(max_bytes).with_max_fields(max_fields);
    }

    constexpr limit() noexcept = default;

    constexpr limit(const limit &) noexcept = default;
    constexpr limit(limit &&) noexcept = default;
    constexpr limit &operator=(const limit &) noexcept = default;
    constexpr limit &operator=(limit &&) noexcept = default;

    constexpr ~limit() noexcept = default;

    constexpr limit with_max_fields(size_t max) const noexcept
    {
        limit ret = *this;
        ret.m_max_fields = max;
        return ret;
    }

    // Set the recursion-depth budget consumed by `on_submessage`.
    constexpr limit with_max_depth(uint32_t max) const noexcept
    {
        limit ret = *this;
        ret.m_max_depth = max;
        return ret;
    }

    constexpr limit with_hard_limit(size_t max_bytes) const noexcept
    {
        limit ret = with_max_bytes(max_bytes);
        ret.m_error_on_bytes = PPB_ERROR_LIMIT_EXCEEDED;
        return ret;
    }

    constexpr limit with_soft_limit(size_t max_bytes) const noexcept
    {
        limit ret = with_max_bytes(max_bytes);
        ret.m_error_on_bytes = PPB_OK;
        return ret;
    }

    // Sets the byte cap without changing the byte-limit error policy.
    // On a default-constructed limit this produces a soft limit;
    // chain after `with_hard_limit`/`with_soft_limit` to override
    // only the cap.
    constexpr limit with_max_bytes(size_t max_bytes) const noexcept
    {
        limit ret = *this;
        ret.m_max_bytes = max_bytes;
        return ret;
    }

    constexpr size_t fields() const noexcept { return m_max_fields; }
    constexpr size_t bytes() const noexcept { return m_max_bytes; }
    constexpr ppb_error error_on_bytes() const noexcept { return m_error_on_bytes; }
    // Accessor (mirrors `fields()`/`bytes()` short-name convention; the
    // static `limit::max_depth(N)` factory occupies the long name).
    constexpr uint32_t depth() const noexcept { return m_max_depth; }

private:
    size_t m_max_fields = std::numeric_limits<size_t>::max();
    size_t m_max_bytes = std::numeric_limits<size_t>::max();
    ppb_error m_error_on_bytes = PPB_OK;
    uint32_t m_max_depth = 0;
};

// Wraps `handler` as a value handler bound to (Key, wire).
//
// `reader::parse()` (and `prescan` / `lexn` / `dispatch`) invokes the
// handler when a decoded field matches.  The handler function must be
// invocable with whatever the matched field's `extract_value`
// returns: typed scalars yield typed arguments, `varint`/`i64`/`len`/
// `i32` yield `const ppb_field &`, and packed varint fields yield a
// lazy view (see `packed_int32` etc.).
//
// The `Key` template argument must have the same C++ type as the
// schema's `Key`: mixing a plain integer literal with an
// `enum class`-keyed schema (or vice versa) is a compile error.
//
// `wire` defaults to `wire_type::any`, which matches every wire type
// associated with `Key` in the schema.  Specify a concrete wire type
// to disambiguate when a schema lists the same key under multiple
// wire types.
//
// Each handler returns `ppb_error`; non-`PPB_OK` returns are
// accumulated into the reader's sticky error (first-write-wins) and
// stop further lexn batches, but every handler in the *current* batch
// still runs.
template <auto Key, wire_type wire = wire_type::any, typename Fn>
[[nodiscard]] constexpr detail::value_handler<Key, wire, std::decay_t<Fn>>
on(Fn &&handler)
{
    return detail::value_handler<Key, wire, std::decay_t<Fn>> { {}, std::forward<Fn>(handler) };
}

// Handler factory for a length-prefixed sub-message field at `Key`.
// The wrapper auto-constructs a nested `reader<InnerSchema>` over the
// payload, derives an inner `limit` (depth budget decremented from the
// outer bounds, hard byte cap = payload size), and dispatches the
// supplied inner handlers via the inner reader's `parse()`.
//
// Two overloads:
//   on_submessage<K, S>(handlers...)         no init
//   on_submessage<K, S>(init, handlers...)   `init` runs after the
//                                            inner prescan with inner
//                                            `meta<...>()` populated,
//                                            so the user can size
//                                            destination storage
//                                            before handlers run.
//
// `K` must match a LEN-wire field in the outer schema (`bytes`,
// `utf8string`, `len`, `unpacked_bytes`, or `message<K, S>`).  When the
// schema field is `ppb::message<K, S2>`, a compile-time static_assert
// in dispatch verifies `S == S2`; raw LEN field types skip the check.
//
// User-supplied `limit` is intentionally not accepted: the wrapper
// owns inner bounds.  Outer `limit::max_depth` must be set to a
// positive value (default 0 fails closed with
// `PPB_ERROR_DEPTH_EXCEEDED`).
template <auto Key, typename InnerSchema, typename... Hs>
    requires(detail::is_handler_v<Hs> && ...)
[[nodiscard]] constexpr auto
on_submessage(Hs &&...inner_handlers)
{
    return detail::submessage_handler<Key, InnerSchema, std::nullopt_t, std::decay_t<Hs>...> {
        std::nullopt,
        std::tuple<std::decay_t<Hs>...>(std::forward<Hs>(inner_handlers)...),
    };
}

template <auto Key, typename InnerSchema, typename Init, typename... Hs>
    requires(!detail::is_handler_v<Init> && (detail::is_handler_v<Hs> && ...))
[[nodiscard]] constexpr auto
on_submessage(Init &&init, Hs &&...inner_handlers)
{
    return detail::submessage_handler<Key, InnerSchema, std::decay_t<Init>, std::decay_t<Hs>...> {
        std::forward<Init>(init),
        std::tuple<std::decay_t<Hs>...>(std::forward<Hs>(inner_handlers)...),
    };
}

// Handler shortcuts for common destination patterns.
// The destination must outlive the parse() / lexn() / dispatch() call.
template <auto Key, wire_type wire = wire_type::any, typename T>
[[nodiscard]] constexpr auto
store(T *dst)
{
    return on<Key, wire>(
        [dst](auto v) -> ppb_error
        {
            (*dst) = std::move(v);
            return PPB_OK;
        });
}

template <auto Key, wire_type wire = wire_type::any, typename C>
[[nodiscard]] constexpr auto
push_back(C *container)
{
    return on<Key, wire>(
        [container](auto v) -> ppb_error
        {
            container->push_back(std::move(v));
            return PPB_OK;
        });
}

template <auto Key, wire_type wire = wire_type::any, typename C>
[[nodiscard]] constexpr auto
emplace_back(C *container)
{
    return on<Key, wire>(
        [container](auto v) -> ppb_error
        {
            container->emplace_back(std::move(v));
            return PPB_OK;
        });
}

// Wraps `handler` as a catch-all (unknown-tag) handler.  Fires once
// per occurrence for any tag that matches a `ppb::unknown<wire>`
// entry in the schema (i.e., any tag the schema doesn't otherwise
// recognize, with the given wire type).
//
// The handler is invoked with `const ppb_field &`; `field.v.ptr`
// points at the original tag byte in the input buffer, so callers
// who need the original field number can recover it via
// `ppb_decode_varint`.
//
// `wire` defaults to `wire_type::any`, matching every catch-all wire
// type the schema registered.  Specify a concrete wire type to bind
// to one bucket; mixing per-wire `on_unknown` factories in a single
// `parse()` call is supported.
//
// Like value handlers, the return value is folded first-write-wins
// into the reader's sticky error.
template <wire_type wire = wire_type::any, typename Fn>
[[nodiscard]] constexpr detail::unknown_handler<wire, std::decay_t<Fn>>
on_unknown(Fn &&handler)
{
    return detail::unknown_handler<wire, std::decay_t<Fn>> { {}, std::forward<Fn>(handler) };
}

// Stateful reader for a schema and a byte span.
//
// Holds a span of unconsumed input, a sticky `ppb_error`, an optional
// `error_field` (set when `field_semantics::error` triggers), and an
// array of per-field state populated by prescan / lexn.  The reader
// itself is trivially copyable; copies are independent snapshots that
// don't share input ownership (there is none) nor decoded state.
//
// Lifecycle:
//   1. Construct with the input bytes.
//   2. Call `parse()` (or the lower-level `prescan` / `lexn`) with
//      `ppb::on<>(...)` handlers.
//   3. To process the next message with the same reader, point the
//      reader at the new bytes and call `reset_fields()`; see
//      `reset_fields()` for the sticky-error caveat.
//
// The `error()`, `error_field()`, and `unknown_field()` state is
// sticky.  Construct a fresh `reader` to clear that state.
template <typename Schema> struct reader;
template <typename... Fs> struct reader<schema<Fs...>>
{
    using Schema = schema<Fs...>;

    constexpr reader() noexcept = default;

    // Constructs a reader over `input`.  Spans larger than
    // `PTRDIFF_MAX` are rejected at construction by setting the
    // sticky error to `PPB_ERROR_TRUNCATED_DATA` (the C lexer's
    // invariant requires `size <= PTRDIFF_MAX`).
    constexpr reader(std::span<const std::byte> input) noexcept
        : m_input(input)
    {
        if (m_input.size() > size_t(std::numeric_limits<ptrdiff_t>::max())) [[unlikely]]
            m_error = PPB_ERROR_TRUNCATED_DATA;
    }

    constexpr reader(const void *input, size_t length) noexcept
        : reader(std::span(reinterpret_cast<const std::byte *>(input), length))
    {
    }

    // Movable and copyable
    constexpr reader(const reader &) noexcept = default;
    constexpr reader(reader &&) noexcept = default;
    constexpr reader &operator=(const reader &) noexcept = default;
    constexpr reader &operator=(reader &&) noexcept = default;

    constexpr ~reader() noexcept = default;

    // Returns the reader's sticky error (initially `PPB_OK`).  Once set
    // to a non-`PPB_OK` value, the error stays set: subsequent calls to
    // `prescan`, `lexn`, `parse`, and `dispatch` short-circuit and
    // return that error without further work.  `reset_fields()` does
    // *not* clear it; use a fresh reader to recover.
    //
    // **Once the error is non-zero, we stop trying to do additional work!**
    [[nodiscard]] constexpr ppb_error error() const noexcept { return m_error; }

    // Returns the *unconsumed* portion of the input, not the original
    // span.  After a `lexn`/`parse`, the prefix that was successfully
    // lexed is no longer reachable from the reader.  Use
    // `size() == 0` / `empty()` to detect end-of-input.
    [[nodiscard]] constexpr std::span<const std::byte> input() const noexcept { return m_input; }
    [[nodiscard]] constexpr size_t size() const noexcept { return m_input.size(); }
    [[nodiscard]] constexpr bool empty() const noexcept { return m_input.empty(); }

    // Encoded tag of the field that triggered `field_semantics::error`,
    // or `nullopt` if no such trigger has occurred.  The encoding matches
    // the protobuf wire tag: `field_number * 8 + wire_type`.  Only set by
    // `parse()` for `field_semantics::error`; not cleared by `reset_fields()`.
    //
    // Returns an `optional<uint64_t>` for regularity, but
    // `ppb::field`s guarantee at compile-time that the return value
    // would always fits in a `uint32_t`.
    [[nodiscard]] constexpr std::optional<uint64_t> error_field() const noexcept
    {
        if (m_error_field != 0)
            return m_error_field;

        return {};
    }

    // Decoded encoded tag (`field_number * 8 + wire_type`) of the first
    // catch-all (`ppb::unknown<wire>`) entry that saw at least one
    // occurrence, or `nullopt` if none did.  Only populated for schemas
    // that actually register catch-alls (e.g. via
    // `ppb::detect_unknown_fields`); never cleared by `reset_fields()`.
    //
    // Like `error_field()`, the value mirrors the protobuf wire-tag
    // encoding so callers can extract `field_number = tag >> 3` and
    // `wire_type = tag & 7`.  Recovered by re-decoding the varint at
    // the catch-all's stored `v.ptr`, since the C lexer rewrites the
    // tag to a wire-only sentinel in the per-field state.
    //
    // Sticky and first-write-wins: once set, subsequent unknowns of any
    // wire type leave the value alone.  Detection is "soft" -- it does
    // *not* set the reader's sticky error or abort dispatch; callers
    // who want a hard error should layer their own check on top of
    // `parse()` returning successfully.
    //
    // Populated by `parse()` and `prescan()`, *not* by the standalone
    // `lexn()`.
    [[nodiscard]] constexpr std::optional<uint64_t> unknown_field() const noexcept
    {
        if (m_unknown_field != 0)
            return m_unknown_field;

        return {};
    }

    // Zeroes the per-field metadata array so the reader can process
    // a fresh message.
    //
    // Does *not* reset `error()` or `error_field()`: the sticky-error
    // model is intentional, and a reader that has already failed cannot
    // be revived this way.  Construct a new reader instead.
    //
    // Typical loop:
    //
    //   ppb::reader<S> r(first_message);
    //   for (;;) {
    //       if (r.parse(init, {}, handlers...) != PPB_OK) break;
    //       // ... use decoded values ...
    //       r = ppb::reader<S>(next_message);
    //   }
    //
    // `reset_fields()` itself is mostly useful when re-running
    // prescan/lexn on the same bytes (e.g. different handlers against
    // an unchanged span), not for stream processing.
    constexpr void reset_fields() noexcept { m_fields = {}; }

    // High-level driver: prescan + optional lexn pass + handler dispatch.
    //
    // Accepts five natural call shapes (all equivalent to the last):
    //
    //   parse(handlers...)               no init, default limit
    //   parse(limit, handlers...)        no init, custom limit
    //   parse(init, handlers...)         init, default limit
    //   parse(limit, init, handlers...)  limit-first, then init
    //   parse(init, limit, handlers...)  existing canonical form
    //
    // `std::nullopt` as init and `{}` as limit still work.
    //
    // 1. Run `ppb_prescan` over the input span subject to `bounds`.
    // 2. Inspect each schema entry's `field_semantics`:
    //      - `error` fields seen on the wire: parse() fails with
    //        `PPB_ERROR_CORRUPT_TAG`, `error_field()` is set, `init` is
    //        not called.
    //      - Otherwise determine whether any field forces a lexn pass
    //        (see `field_semantics`).
    // 3. Call `init(std::as_const(*this))`.  Use the snapshot to inspect
    //    `meta<Key>()` and preallocate per-message storage.  If `init`
    //    returns non-`PPB_OK`, the error is recorded and the input span
    //    is *not* advanced; the next call to `parse()` will short-circuit
    //    on the sticky error.  Pass `std::nullopt` as init to no-op.
    // 4. If no lexn pass is needed: dispatch handlers once with the
    //    prescan-aggregated values, then advance the input past the
    //    prescanned bytes.
    // 5. Otherwise: run `ppb_lexn` in a loop, dispatching handlers per
    //    batch.  The input span advances as bytes are consumed; on a
    //    handler error or lexn error the loop stops with the input span
    //    pointing past the last fully-consumed batch.
    //
    // Exits early with `PPB_OK` (without invoking `init`) when prescan
    // reports zero bytes, e.g., on empty input, `limit::max_fields(0)`,
    // or a soft byte limit at offset zero.  Callers who need `init` to
    // run even on empty input should special-case that themselves.
    //
    // Handlers' return values follow the same first-write-wins
    // accumulation as `lexn`: every handler in the current batch is
    // invoked even if an earlier one errored.
    //
    // *Consumes* from the input span!
    template <typename Init, typename... Hs>
        requires(!detail::is_handler_v<Init> && !std::convertible_to<std::decay_t<Init>, limit> &&
            (detail::is_handler_v<Hs> && ...))
    [[nodiscard]] ppb_error parse(Init &&init, limit bounds = {}, Hs &&...handlers)
    {
        std::tuple<std::decay_t<Hs>...> tup(std::forward<Hs>(handlers)...);
        return parse_tuple(std::forward<Init>(init), bounds, tup);
    }

    // parse(handlers...)  -> parse(nullopt, {}, handlers...)
    template <typename... Hs>
        requires(sizeof...(Hs) >= 1 && (detail::is_handler_v<Hs> && ...))
    [[nodiscard]] ppb_error parse(Hs &&...handlers)
    {
        return parse(std::nullopt, limit {}, std::forward<Hs>(handlers)...);
    }

    // parse(limit, handlers...)  -> parse(nullopt, limit, handlers...)
    template <typename... Hs>
        requires(sizeof...(Hs) >= 1 && (detail::is_handler_v<Hs> && ...))
    [[nodiscard]] ppb_error parse(limit bounds, Hs &&...handlers)
    {
        return parse(std::nullopt, bounds, std::forward<Hs>(handlers)...);
    }

    // parse(init, handlers...)  -> parse(init, {}, handlers...)
    template <typename Init, typename... Hs>
        requires(!std::convertible_to<std::decay_t<Init>, limit>) && (!detail::is_handler_v<Init>) &&
        (sizeof...(Hs) >= 1) && (detail::is_handler_v<Hs> && ...)
    [[nodiscard]] ppb_error parse(Init &&init, Hs &&...handlers)
    {
        return parse(std::forward<Init>(init), limit {}, std::forward<Hs>(handlers)...);
    }

    // parse(limit, init, handlers...)  -> parse(init, limit, handlers...)
    template <typename Init, typename... Hs>
        requires(!std::convertible_to<std::decay_t<Init>, limit>) && (!detail::is_handler_v<Init>) &&
        (sizeof...(Hs) >= 1) && (detail::is_handler_v<Hs> && ...)
    [[nodiscard]] ppb_error parse(limit bounds, Init &&init, Hs &&...handlers)
    {
        return parse(std::forward<Init>(init), bounds, std::forward<Hs>(handlers)...);
    }

    // Tuple-accepting variant of `parse(init, bounds, handlers...)`:
    // lets callers directly reuse a pre-built handler tuple without
    // unpacking via `std::apply`.  The tuple is taken by mutable
    // reference to enable stateful handler callbacks.
    template <typename Init, typename... Hs>
        requires(detail::is_handler_v<Hs> && ...)
    [[nodiscard]] ppb_error parse_tuple(Init &&init, limit bounds, std::tuple<Hs...> &handlers);

    // Runs `ppb_prescan` over the input span subject to `bounds`.  When
    // any `on()` handlers are passed, dispatches them once using the
    // prescan-aggregated values (last-occurrence semantics for scalars;
    // LEN payloads are the last seen on the wire).
    //
    // Returns the number of bytes prescanned (>= 0) on success, or a
    // negative `ppb_error` value.  Does *not* advance the input span;
    // call `lexn`/`parse` to consume bytes.
    //
    // Handler errors are folded into the reader's sticky error; the
    // returned value reflects them as well (negative on error).
    template <typename... Hs> [[nodiscard]] ptrdiff_t prescan(limit bounds = {}, Hs &&...handlers);

    // Returns the prescan-aggregated `ppb_field_meta` for `key`.
    //
    // By default merges metadata across all schema entries that match
    // `key` (i.e., the same field number under different wire types);
    // pass an explicit `wire` to read just one entry's metadata.
    //
    // Compile-error when `key` (or the `(key, wire)` pair) is not in the
    // schema.  Before the first `prescan`/`parse`, returns a
    // zero-initialized `ppb_field_meta`.
    template <typename Schema::Key key, wire_type wire = wire_type::any>
    constexpr ppb_field_meta meta() const noexcept;

    // Runs `ppb_lexn` once over the input span subject to `bounds`.
    // Decodes a batch of strictly monotonically-increasing fields; on
    // return the input span has advanced past the consumed bytes (even
    // if `bounds` triggered the early exit, and even on a non-fatal
    // limit error).
    //
    // When any `on()` handlers are passed, dispatches them for fields in
    // the decoded batch.  Handler errors fold into the reader's sticky
    // error, but every handler in the batch still runs.
    //
    // Call in a loop until `empty()` (or until `error()` is non-OK) to
    // process a whole message.  Consider `lex_all()` for the common case
    // of draining the input with repeated `lexn()` batches.
    //
    // *Consumes* from the input span!
    template <typename... Hs> [[nodiscard]] ppb_error lexn(limit bounds = {}, Hs &&...handlers)
    {
        std::tuple<std::decay_t<Hs>...> tup(std::forward<Hs>(handlers)...);
        return lexn_tuple(bounds, tup);
    }

    template <typename... Hs> [[nodiscard]] ppb_error lexn_tuple(limit bounds, std::tuple<Hs...> &handlers);

    // Repeatedly calls `lexn()` until the input is empty or an error is
    // set.  `bounds` applies *per batch*, not cumulatively.
    //
    // Handler dispatch and the sticky-error contract are identical to
    // `lexn()`.
    template <typename... Hs> [[nodiscard]] ppb_error lex_all(Hs &&...handlers)
    {
        return lex_all(limit {}, std::forward<Hs>(handlers)...);
    }

    // Returns PPB_OK when bounds forbids any progress (byte or field limit of 0).
    template <typename... Hs> [[nodiscard]] ppb_error lex_all(limit bounds, Hs &&...handlers)
    {
        std::tuple<std::decay_t<Hs>...> tup(std::forward<Hs>(handlers)...);
        return lex_all_tuple(bounds, tup);
    }

    // Returns PPB_OK when bounds forbids any progress (byte or field limit of 0).
    template <typename... Hs> [[nodiscard]] ppb_error lex_all_tuple(limit bounds, std::tuple<Hs...> &tup);

    // Re-dispatches handlers against the current per-field state without
    // touching the input span.  Useful for testing and for invoking
    // additional handlers after a prescan, but not part of the normal
    // parse loop.
    //
    // `dispatch_tuple` for generic usage; direct invocations should
    // probably prefer `dispatch`.  When `run_zero_defaults = true`,
    // invokes handlers for proto3_zero_default fields
    // unconditionally (otherwise, treats them like `last_write_wins`).
    template <typename... Hs> ppb_error dispatch(Hs &&...handlers);

    template <typename... Hs>
    ppb_error dispatch_tuple(std::tuple<Hs...> &handlers, bool run_zero_defaults = false,
        const limit &bounds = limit {});

private:
    constexpr ppb_buf make_ppb_buf() const noexcept
    {
        return ppb_buf {
            .buf = m_input.data(),
            .size = m_input.size(),
        };
    }

    template <typename... Hs>
    [[gnu::noinline]] void run_handlers(std::tuple<Hs...> &handlers, const limit &active_bounds,
        const std::byte *outer_end, uintptr_t lower_bound, uintptr_t range_size_inclusive, size_t begin = 0,
        size_t end = Schema::num_fields(), bool run_zero_defaults = false);

    template <size_t idx, typename... Hs>
    [[gnu::always_inline]] inline void run_handler_for_idx(std::tuple<Hs...> &handlers,
        const limit &active_bounds, const std::byte *outer_end, uintptr_t lower_bound,
        uintptr_t range_size_inclusive, bool run_zero_defaults);

    // Walks the schema for catch-all entries (`ppb::unknown<wire>`) and
    // updates `m_unknown_field` first-write-wins.  Compiles to nothing
    // for schemas that don't register catch-alls.
    //
    // For each catch-all that fired, we re-decode the original tag
    // varint at `field.v.ptr`
    void detect_unknown_field() noexcept
    {
        if (m_unknown_field != 0)
            return;

        [&]<size_t... Is>(std::index_sequence<Is...>)
        {
            (
                [&]<size_t I>(std::integral_constant<size_t, I>)
                {
                    using Field = decltype(Schema::template field<I>());
                    Field::maybe_flag_unknown_field(m_fields[I], m_input, &m_unknown_field);
                }(std::integral_constant<size_t, Is> {}),
                ...);
        }(std::make_index_sequence<Schema::num_fields()> {});
    }

    std::span<const std::byte> m_input;
    ppb_error m_error = PPB_OK;
    // m_error_field and m_unknown_field are initially zero, when we
    // haven't detected any error or unknown field (a zero field would
    // be rejected at compile-time for a field, or at runtime as
    // invalid wire encoding during prescan or lexn).
    //
    // m_error_field is a uint32_t because we check that fields fit in
    // that range (tag < 2**29) at compile-time; no such guarantee for
    // arbitrary fields on the wire (we could reject large tag values
    // as invalid encoding too, but we currently don't).
    //
    // First non-zero value is sticky.
    uint32_t m_error_field = 0;
    uint64_t m_unknown_field = 0;
    std::array<ppb_field, Schema::num_fields()> m_fields = {};
};

/*
 * End of public interface.  Out-of-line definitions follow.
 */

template <typename... Fs>
template <typename Init, typename... Hs>
    requires(detail::is_handler_v<Hs> && ...)
[[nodiscard]] ppb_error
reader<schema<Fs...>>::parse_tuple(Init &&init, limit bounds, std::tuple<Hs...> &tup)
{
    if (m_error != PPB_OK) [[unlikely]]
        return m_error;

    ppb_buf buf = make_ppb_buf();

    ptrdiff_t num_bytes = ppb_prescan_impl(buf, m_fields.size(), Schema::s_encoded_tags.data(),
        m_fields.data(), bounds.fields(), bounds.bytes(), bounds.error_on_bytes());

    if (num_bytes <= 0) [[unlikely]]
    {
        m_error = ppb_error(num_bytes);
        return m_error;
    }

    detect_unknown_field();

    // Check field semantics: error fields that appeared on the wire
    // are an immediate failure.  Also determine whether any field
    // forces a lexn pass.
    bool has_error = false;
    bool need_lexn = false;
    [&]<size_t... Is>(std::index_sequence<Is...>)
    {
        // Compile-time check: does the handler pack carry a handler
        // for this schema field?  We use this to skip the lexn-pass
        // forcing logic for fields that wouldn't dispatch anyway --
        // forcing lexn only to silently drop every occurrence is pure
        // waste.  Always-on `field_semantics::error` checking is
        // independent and stays unconditional.
        constexpr auto has_handler_for = []<typename Field>() consteval
        {
            if constexpr (Field::is_unknown())
            {
                return detail::find_unknown_handler<Field::wire(), const ppb_field &, std::decay_t<Hs>...>()
                    .has_value();
            }
            else
            {
                using V = decltype(Field::extract_value(std::declval<const ppb_field &>(),
                    std::declval<ppb_error *>()));
                return detail::find_value_handler<Field::tag(), Field::wire(), V, std::decay_t<Hs>...>()
                    .has_value();
            }
        };

        auto scan = [&]<size_t I>(std::integral_constant<size_t, I>)
        {
            using Field = decltype(Schema::template field<I>());
            constexpr bool has_handler = has_handler_for.template operator()<Field>();
            std::optional<uint32_t> err =
                Field::template classify_field_before_lexn<has_handler>(m_fields[I].m, &need_lexn);
            if (err.has_value()) [[unlikely]]
            {
                has_error = true;
                m_error_field = *err;
            }
        };
        (scan(std::integral_constant<size_t, Is> {}), ...);
    }(std::make_index_sequence<Schema::num_fields()> {});

    if (has_error) [[unlikely]]
    {
        m_error = PPB_ERROR_CORRUPT_TAG;
        return m_error;
    }

    if constexpr (!std::is_same_v<std::decay_t<Init>, std::nullopt_t>)
    {
        m_error = init(std::as_const(*this));
    }

    if (m_error != PPB_OK || num_bytes == 0) [[unlikely]]
        return m_error;

    const std::byte *const outer_end = m_input.data() + m_input.size();

    if (!need_lexn) [[likely]]
    {
        run_handlers(tup, bounds, outer_end, reinterpret_cast<uintptr_t>(m_input.data()),
            size_t(num_bytes) - 1, 0, Schema::num_fields(), /*run_zero_defaults=*/true);
        m_input = m_input.subspan(size_t(num_bytes));
        return m_error;
    }

    const std::byte *const logical_end = m_input.data() + num_bytes;
    const std::byte *base;

    while ((base = reinterpret_cast<const std::byte *>(buf.buf)) < logical_end)
    {
        ppb_lexn_ret ret = ppb_lexn_with_hard_limit(&buf, size_t(logical_end - base), m_fields.size(),
            Schema::s_encoded_tags.data(), m_fields.data(), std::numeric_limits<size_t>::max());

        size_t range_size = size_t(reinterpret_cast<const std::byte *>(buf.buf) - base);

        // look for an error (prescan should have caught any wire encoding issue,
        // but maybe the bytes changed along the way... `range_size == 0` can only
        // happen on error (we already checked the input is before the hard limit,
        // and the C library guarantees progress), but a little defensiveness is good
        if (ret.status != PPB_OK || range_size == 0) [[unlikely]]  // LCOV_EXCL_LINE
        {
            m_error = ret.status;
            break;
        }

        run_handlers(tup, bounds, outer_end, reinterpret_cast<uintptr_t>(base), range_size - 1,
            ret.first_field, ret.first_field + ret.field_range);
        if (m_error != PPB_OK) [[unlikely]]
            break;
    }

    m_input = std::span(reinterpret_cast<const std::byte *>(buf.buf), buf.size);
    return m_error;
}

template <typename... Fs>
template <typename... Hs>
[[nodiscard]] ptrdiff_t
reader<schema<Fs...>>::prescan(limit bounds, Hs &&...handlers)
{
    if (m_error != PPB_OK) [[unlikely]]
        return m_error;

    ptrdiff_t ret = ppb_prescan_impl(make_ppb_buf(), m_fields.size(), Schema::s_encoded_tags.data(),
        m_fields.data(), bounds.fields(), bounds.bytes(), bounds.error_on_bytes());

    if (ret <= 0) [[unlikely]]
    {
        m_error = ppb_error(ret);
        return m_error;
    }

    detect_unknown_field();

    if constexpr (sizeof...(Hs) == 0)
        return ret;

    std::tuple<std::decay_t<Hs>...> tup(std::forward<Hs>(handlers)...);
    ppb_error err = dispatch_tuple(tup, /*run_zero_defaults=*/true, bounds);
    return int(err) < 0 ? ptrdiff_t(err) : ret;
}

template <typename... Fs>
template <typename schema<Fs...>::Key key, wire_type wire>
constexpr ppb_field_meta
reader<schema<Fs...>>::meta() const noexcept
{
    constexpr typename Schema::field_range range = Schema::find_field_range(key, wire);

    static_assert(range.count > 0, "Key not found in schema");
    static_assert(range.begin < Schema::num_fields(), "range (lo) must be in bounds");
    static_assert(Schema::num_fields() - range.begin >= range.count, "range (hi) must be in bounds");

    ppb_field_meta ret = m_fields[range.begin].m;
    for (size_t idx = 1; idx < range.count; idx++)
    {
        detail::merge_meta(ret, m_fields[range.begin + idx].m);
    }

    return ret;
}

template <typename... Fs>
template <typename... Hs>
[[nodiscard]] ppb_error
reader<schema<Fs...>>::lexn_tuple(limit bounds, std::tuple<Hs...> &tup)
{
    if (m_error != PPB_OK) [[unlikely]]
        return m_error;

    const auto base = reinterpret_cast<uintptr_t>(m_input.data());

    ppb_buf buf = make_ppb_buf();
    const ppb_lexn_ret ret = ppb_lexn_impl(&buf, m_fields.size(), Schema::s_encoded_tags.data(),
        m_fields.data(), bounds.fields(), bounds.bytes(), bounds.error_on_bytes());

    m_error = ret.status;
    m_input = std::span(reinterpret_cast<const std::byte *>(buf.buf), buf.size);

    size_t range_size = reinterpret_cast<uintptr_t>(buf.buf) - base;
    if constexpr (sizeof...(Hs) == 0)
    {
        return m_error;
    }

    // range_size == 0 should only happen on error or empty input.
    if (m_error != PPB_OK || range_size == 0) [[unlikely]]
    {
        return m_error;
    }

    run_handlers(tup, bounds, m_input.data() + m_input.size(), base, range_size - 1, ret.first_field,
        ret.first_field + ret.field_range);
    return m_error;
}

template <typename... Fs>
template <typename... Hs>
[[nodiscard]] ppb_error
reader<schema<Fs...>>::lex_all_tuple(limit bounds, std::tuple<Hs...> &tup)
{
    if (bounds.bytes() == 0 || bounds.fields() == 0) [[unlikely]]
        return m_error;

    while (!m_input.empty())
    {
        ppb_error ret = lexn_tuple(bounds, tup);
        if (ret != PPB_OK)
            return ret;
    }

    return m_error;
}

template <typename... Fs>
template <typename... Hs>
ppb_error
reader<schema<Fs...>>::dispatch(Hs &&...handlers)
{
    std::tuple<std::decay_t<Hs>...> tup(std::forward<Hs>(handlers)...);
    return dispatch_tuple(tup);
}

template <typename... Fs>
template <typename... Hs>
ppb_error
reader<schema<Fs...>>::dispatch_tuple(std::tuple<Hs...> &handlers, bool run_zero_defaults,
    const limit &bounds)
{
    if constexpr (sizeof...(Hs) == 0)
        return m_error;

    if (m_error != PPB_OK) [[unlikely]]
        return m_error;

    run_handlers(handlers, bounds, m_input.data() + m_input.size(), /*lower_bound=*/1,
        /*range_size_inclusive=*/std::numeric_limits<uintptr_t>::max() - 1,
        /*begin=*/0, /*end=*/Schema::num_fields(), run_zero_defaults);
    return m_error;
}

template <typename... Fs>
template <typename... Hs>
[[gnu::noinline]] void
reader<schema<Fs...>>::run_handlers(std::tuple<Hs...> &handlers, const limit &active_bounds,
    const std::byte *outer_end, uintptr_t lower_bound, uintptr_t range_size_inclusive, size_t begin,
    size_t end, bool run_zero_defaults)
{
    static_assert((detail::handler_matches_some_field<Hs, Fs...>() && ...),
        "on<Key> or on_unknown<> handler does not match any schema field; the "
        "key is absent from the schema / there is no ppb::unknown<> field in the "
        "schema (respectively), or the key exists but wire types don't match.");

    detail::dispatch<Schema::num_fields()>(begin, end,
        [&]<size_t I>(std::integral_constant<size_t, I>)
        {
            run_handler_for_idx<I>(handlers, active_bounds, outer_end, lower_bound, range_size_inclusive,
                run_zero_defaults);
            return true;
        });
}

template <typename... Fs>
template <size_t idx, typename... Hs>
[[gnu::always_inline]] void
reader<schema<Fs...>>::run_handler_for_idx(std::tuple<Hs...> &handlers, const limit &active_bounds,
    const std::byte *outer_end, uintptr_t lower_bound, uintptr_t range_size_inclusive, bool run_zero_defaults)
{
    const ppb_field &field = m_fields[idx];

    using Field = decltype(Schema::template field<idx>());

    // Catch-all entries have no `Key`, so they go through the
    // wire-only `find_unknown_handler` machinery instead of
    // `find_value_handler` (which static_asserts on key_type
    // matching the dispatch Key).
    if constexpr (Field::is_unknown())
    {
        constexpr std::optional<size_t> handler_idx =
            detail::find_unknown_handler<Field::wire(), const ppb_field &, Hs...>();
        if constexpr (handler_idx.has_value())
        {
            if (reinterpret_cast<uintptr_t>(field.v.ptr) - lower_bound > range_size_inclusive) [[unlikely]]
                return;

            enum ppb_error result = std::get<handler_idx.value()>(handlers).handler(field);
            if (result != PPB_OK) [[unlikely]]
                m_error = (m_error == PPB_OK) ? result : m_error;
        }
    }
    else
    {
        // Find a handler for the schema field.
        constexpr std::optional<size_t> handler_idx = detail::find_value_handler<Field::tag(), Field::wire(),
            decltype(Field::extract_value(field, &m_error)), Hs...>();

        if constexpr (handler_idx.has_value())
        {
            if constexpr (Field::semantics() == field_semantics::proto3_zero_default)
            {
                // widen range_size_inclusive = UINTPTR_MAX if run_zero_defaults.
                range_size_inclusive |= -uintptr_t(run_zero_defaults);
            }

            if (reinterpret_cast<uintptr_t>(field.v.ptr) - lower_bound > range_size_inclusive) [[unlikely]]
                return;

            using H = std::tuple_element_t<handler_idx.value(), std::tuple<Hs...>>;
            enum ppb_error result;
            if constexpr (H::is_submessage_handler())
            {
                // Proto3 for submessages isn't supported because:
                //  1. proto3 doesn't even do that
                //  2. it's probably not faster to parse a potentially empty message than to
                //     conditionally branch over the whole thing
                //  3. the way we extend the input span<> and truncate with a hard limit doesn't
                //     work when the span's base pointer lies outside the input span.
                static_assert(Field::semantics() != field_semantics::proto3_zero_default,
                    "submessages are incompatible with proto3_zero_default (proto3 preserves proto2 last_write_wins for messages)");

                if constexpr (detail::is_message_field_v<Field>)
                {
                    static_assert(std::is_same_v<typename Field::inner_schema, typename H::inner_schema_type>,
                        "ppb::message<K, S> field paired with ppb::on_submessage<K, S2> requires S == S2");
                }

                std::span<const std::byte> payload(static_cast<const std::byte *>(field.v.payload.buf),
                    field.v.payload.size);

                result = std::get<handler_idx.value()>(handlers).invoke(payload, active_bounds, outer_end);
            }
            else
            {
                result = std::get<handler_idx.value()>(handlers).handler(
                    Field::extract_value(field, &m_error));
            }

            if (result != PPB_OK) [[unlikely]]
                m_error = (m_error == PPB_OK) ? result : m_error;
        }
    }
}

// Out-of-line definition of `submessage_handler::invoke`: needs the
// full definitions of `ppb::reader` and `ppb::limit`, which are
// introduced after `ppb_detail.hpp` is included.
template <auto K, typename InnerSchema, typename Init, typename... Hs>
[[gnu::always_inline]] inline ppb_error
detail::submessage_handler<K, InnerSchema, Init, Hs...>::invoke(std::span<const std::byte> payload,
    const limit &active_bounds, const std::byte *outer_end)
{
    if (active_bounds.depth() == 0) [[unlikely]]
        return PPB_ERROR_DEPTH_EXCEEDED;

    const limit inner_bounds =
        limit {}.with_max_depth(active_bounds.depth() - 1).with_hard_limit(payload.size());

    // Suffix of the outer input span starting at the payload: extends
    // through `outer_end` so the C lexer's unguarded multi-byte-ahead
    // reads stay in-bounds; the hard byte limit caps logical work at
    // `payload.size()`.
    const std::byte *const suffix_data = payload.data();
    const size_t suffix_size = static_cast<size_t>(outer_end - suffix_data);

    reader<InnerSchema> inner(std::span<const std::byte>(suffix_data, suffix_size));
    return inner.parse_tuple(init_cb, inner_bounds, inner_handlers);
}

}  // namespace ppb
