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
// over the C lexer in <ppb/ppb.h>.  See `README_CPP.md` for a usage
// guide; this header is the reference.
//
// The wrapper compiles a typed `ppb::schema<...>` down to the
// encoded-tag arrays the C library consumes, then dispatches matched
// fields to `ppb::on<Key>(callable)` handlers with typed C++
// arguments.  Like the C library, nothing here allocates: callers
// pass in a byte span, and decoded values either point back into that
// span or are returned by value.  `ppb::schema<>` performs the
// validation that `ppb_validate_tags` does at runtime, so the C check
// is not needed.
//
// Public API at a glance:
//   - `ppb::schema<Fs...>` -- compile-time-validated field type list.
//   - `ppb::auto_schema<Ts...>` -- flatten and sort fields from tuples
//     into a `schema<...>`.
//   - `ppb::detect_unknown_fields<sem>` -- catch-all tuple for all four wire
//     types; drop into `auto_schema` to opt into unknown-field reporting.
//     `sem` defaults to `field_semantics::repeated`; use `field_semantics::error`
//     to automatically report unknown fields as errors (with `reader::parse` only).
//   - `ppb::reader<Schema>` -- stateful driver over a byte span.
//   - `reader::parse(...)` -- prescan, optional lexn pass, dispatch.
//     Lower-level `prescan`, `lexn`, `lex_all`, `dispatch` are also
//     exposed.  `lexn` and `lex_all` advance the input span
//     unconditionally, including on non-fatal limit errors.
//   - `ppb::on<Key>(callable)`, `ppb::on_submessage<K, S>(...)`,
//     `ppb::on_unknown<wire>(callable)` -- handler factories.
//   - `ppb::store`, `ppb::push_back`, `ppb::emplace_back` -- handler
//     shortcuts for common destination patterns.
//   - `ppb::limit` -- byte / field caps and `on_submessage` recursion
//     budget.
//
// Field descriptors are declared just below: wire-typed primitives
// (`varint`/`i64`/`len`/`i32`), typed scalars (`int32`, `utf8string`,
// `bytes<K, Element>`, ...), packed and unpacked repeated variants,
// `message<K, InnerSchema>` for embedded messages, and
// `unknown<wire>` catch-alls.
//
// Field-tag range is the protobuf [1, 2**29).  Handlers receive a
// typed argument; raw wire-typed primitives instead get
// `const ppb_field &`.  `enumerated<K, Enum>` does *not* range-check
// wire values.
//
// Errors are sticky on the reader: once `error()` is non-`PPB_OK`,
// every subsequent call short-circuits.  Handler return values merge
// first-write-wins into the reader's sticky error, but every handler
// in the current batch still runs.  `reset_fields()` zeros per-field
// state but does *not* clear the sticky error; construct a fresh
// reader to recover.
//
// Compile-time diagnostics: heavy use of `static_assert` catches
// out-of-range tags, bad wire types, mis-ordered or duplicate fields,
// handler key/arg type mismatches, and ambiguous dispatch.  See
// `tests/test_ppb_cpp_static.cc` and `tests/test_ppb_cpp_compile_fail.cc`
// for the full diagnostic catalog.

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

// Per-field policy controlling how `reader::parse()` dispatches when
// a field appears multiple times on the wire.  Prescan metadata is
// aggregated identically regardless of policy.
//
//   always_lexn     Always lexn, even for a single occurrence; handlers
//                   fire in wire order.  Default for raw `varint` /
//                   `i64` / `len` / `i32` types.
//
//   repeated        Dispatch every occurrence: prescan aggregate when
//                   there's one, lexn pass when there are several.
//
//   singular        Last-write-wins when the squashed values are
//                   bit-identical (varint/i32/i64); otherwise lexn so
//                   every occurrence reaches the handler.  LEN payloads
//                   are not compared, so any repeat forces lexn;
//                   prefer `last_write_wins` if you want LEN repeats
//                   silently dropped.
//
//   last_write_wins Only the last occurrence is dispatched.  Matches
//                   proto2 default field semantics for scalars.
//
//   proto3_zero_default Like `last_write_wins`, plus: absent fields
//                   *may be* dispatched with their zero value (0 / empty
//                   span) when `parse()` takes the prescan fast path.
//                   See the `ppb::proto3_<scalar>` aliases.
//
//   error           Any occurrence makes `parse()` return
//                   PPB_ERROR_CORRUPT_TAG, set `reader::error_field()`,
//                   and skip both `init` and handlers.  Only `parse()`
//                   honors this; direct `prescan`/`lexn`/`dispatch`
//                   calls do not flag `error`-semantics fields.
//
// N.B., there is no guarantee for `proto3_zero_default` that the
// handler will or won't be invoked with a zero value for an absent
// field.  That policy is solely useful as a version of
// `last_write_wins` that may enable simpler handler dispatch.
enum class field_semantics : int8_t
{
    always_lexn = -1,  // always force lexn, even for a single occurrence
    repeated = 0,  // we want to see everything
    singular = 1,  // it's ok to do LWW if all the squashed values are equivalent
    last_write_wins = 2,  // we only want to see the last value (regular proto semantics)
    proto3_zero_default = 3,  // LWW but impl may optionally dispatch absent fields as zero/empty
    error = 127,  // just report a PPB_ERROR_CORRUPT_TAG if we see this *in `reader::parse`*
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

// Catch-all descriptor for tags the schema doesn't otherwise list.
// Add one per wire type (or all four via `detect_unknown_fields`) so
// unknown tags are accounted for instead of silently skipped, and so
// `reader::unknown_field()` can report when one was seen.  Carries no
// `Key`, so it composes with any schema key type.
//
// For `field_semantics::error`, the error field tag is reported as
// 2**29 - 1.
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

// Varint-backed enum field.  No range checking: wire values that
// don't name an enumerator reach the handler as the corresponding bit
// pattern.  Prefer a scoped enum or one with an explicit underlying
// type so the cast is well-defined for any wire value.
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
//
// utf8string assumes the payload is valid UTF-8, as required by the protobuf
// spec, but does not check.  Validate yourself if necessary.
template <auto K, field_semantics sem = field_semantics::last_write_wins> struct utf8string;
template <auto K, typename Element = std::byte, field_semantics sem = field_semantics::last_write_wins>
struct bytes;

// Embedded-message field: a LEN payload describing a protobuf-encoded
// message of type `InnerSchema`.  On the wire it's a plain `bytes`
// field; the extra type information lets `ppb::on_submessage<K, S>`
// statically verify that the schema and the handler agree on the
// inner type.  Last-write-wins by default.
//
// N.B., by default, the `limit::max_depth()` is set to 0, so
// `on_submessage` always fails with `PPB_ERROR_DEPTH_EXCEEDED`; relax
// that the recursion depth budget to a strictly positive value to
// enable submessage parsing.
template <auto K, typename InnerSchema, field_semantics sem = field_semantics::last_write_wins>
struct message;

// Convenience aliases for `proto3_zero_default` semantics: same as
// `last_write_wins`, but absent fields are also dispatched with their
// zero value (or empty span) when `parse()` takes the prescan fast
// path.  Matches proto3 default scalar semantics.
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

// Wraps a fixed-width value as little-endian, unaligned bytes (the
// wire format for fixed32/sfixed32/fixed64/sfixed64/float/double).
// Conversion to `T` returns the host-order value.
template <typename T> struct le_packed
{
    static_assert(std::is_trivially_copyable_v<T>,
        "ppb::le_packed requires a trivially copyable element type");
    static_assert(sizeof(T) == 4 || sizeof(T) == 8, "ppb::le_packed only supports 4- or 8-byte values");

    T value() const;
    [[gnu::always_inline]] operator T() const { return value(); }

    std::byte data[sizeof(T)];
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

// Tuple that registers the catch-all for every wire type at once;
// drop into `auto_schema` to opt into unknown-field detection without
// listing the four unknowns explicitly.  Accepts an optional
// `field_semantics` (defaults to `repeated`) to control how unknown
// fields are classified:
//
//   using S = ppb::auto_schema<MyFields..., ppb::detect_unknown_fields<>>;
//   using Strict = ppb::auto_schema<MyFields..., ppb::detect_unknown_fields<field_semantics::error>>;
//
// For `field_semantics::error`, the error field tag is reported as
// 2**29 - 1.
template <field_semantics sem = field_semantics::repeated>
using detect_unknown_fields = std::tuple<unknown<wire_type::varint, sem>, unknown<wire_type::i64, sem>,
    unknown<wire_type::len, sem>, unknown<wire_type::i32, sem>>;

}  // namespace ppb

// clang-format off
#include "ppb_detail.hpp"
// clang-format on

namespace ppb
{

// Compile-time schema: a strictly-ascending type list of field
// descriptors.  All fields must share the same `Key` type (typically
// an `enum class`); `unknown<>` catch-alls have no key and are exempt.
// Tags appear in strictly ascending encoded order; when the same
// field number appears under multiple wire types, list them in
// `enum ppb_wire_type` order (varint, i64, len, i32).
//
// `field<I>()` returns a default-constructed instance of the I-th
// field type (use `decltype(...)` to recover the type).
// `s_encoded_tags` is the parallel array consumed by the C lexer.
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

// Accepts field descriptors and/or (possibly nested)
// `std::tuple<...>`s of descriptors, flattens, sorts ascending by
// `(field_number, wire_type)`, and yields the corresponding
// `ppb::schema<...>`.  Useful when assembling a schema from reusable
// sub-tuples without worrying about tag order:
//
//   using shared = std::tuple<ppb::varint<3>, ppb::len<7>>;
//   using my_schema = ppb::auto_schema<ppb::varint<1>, shared, ppb::i32<2>>;
//   // == ppb::schema<ppb::varint<1>, ppb::i32<2>, ppb::varint<3>, ppb::len<7>>
//
// Duplicates are still rejected by `schema<>`'s ascending check.
template <typename... Ts> using auto_schema = typename detail::sorted_fields<Ts...>::template to<schema>;

// Byte / field / depth caps for `parse`/`prescan`/`lexn`/`lex_all`.
// Default byte limit is soft (stop silently); use `with_hard_limit`
// or the `hard()` factory to make it an error.
//
// Named factories:
//   limit::max_fields(n)         cap toplevel-field count
//   limit::max_depth(n)          recursion budget for `on_submessage`
//   limit::hard(bytes [, n])     stop at `bytes` and report PPB_ERROR_LIMIT_EXCEEDED
//   limit::soft(bytes [, n])     stop at `bytes`, no error
//
// `with_*` builders chain extra caps onto an existing limit.
struct limit
{
public:
    static constexpr limit max_fields(size_t max) noexcept { return limit().with_max_fields(max); }

    // Recursion budget for `on_submessage`.  Default 0 rejects any
    // sub-message traversal with `PPB_ERROR_DEPTH_EXCEEDED`; callers
    // must opt in by setting a positive value.
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

    // Sets the byte cap without changing the byte-limit error policy
    // (soft by default; use `with_hard_limit`/`with_soft_limit` to
    // make the policy explicit).
    constexpr limit with_max_bytes(size_t max_bytes) const noexcept
    {
        limit ret = *this;
        ret.m_max_bytes = max_bytes;
        return ret;
    }

    constexpr size_t fields() const noexcept { return m_max_fields; }
    constexpr size_t bytes() const noexcept { return m_max_bytes; }
    constexpr ppb_error error_on_bytes() const noexcept { return m_error_on_bytes; }
    constexpr uint32_t depth() const noexcept { return m_max_depth; }

private:
    size_t m_max_fields = std::numeric_limits<size_t>::max();
    size_t m_max_bytes = std::numeric_limits<size_t>::max();
    ppb_error m_error_on_bytes = PPB_OK;
    uint32_t m_max_depth = 0;
};

// Wraps `handler` as a value handler bound to `(Key, wire)`.
// Invoked by `parse`/`prescan`/`lexn`/`dispatch` when a decoded
// field matches.  The handler's argument type must match the field's
// decoded value (typed scalars yield typed arguments; raw
// `varint`/`i64`/`len`/`i32` yield `const ppb_field &`; packed varint
// fields yield a lazy view that must not outlive the reader).
//
// When the argument is a `std::span<>` or `std::string_view`, the
// value is borrowed from the input span, and must not outlive that
// span.  When the argument is a `packed_varint_view`, the value
// borrows from the input span *and holds a mutable pointer to the
// `reader<>`*, so must not outlive the callback's invocation.
//
// `Key` must share the schema's `Key` C++ type; mixing integer
// literals with an `enum class`-keyed schema is a compile error.
// `wire` defaults to `wire_type::any`, matching every wire type
// associated with `Key`; specify it to disambiguate.
//
// Handlers return `ppb_error`; non-OK returns fold into the reader's
// sticky error (first-write-wins) and stop further lexn batches, but
// every handler in the current batch still runs.
template <auto Key, wire_type wire = wire_type::any, typename Fn>
[[nodiscard]] constexpr detail::value_handler<Key, wire, std::decay_t<Fn>>
on(Fn &&handler)
{
    return detail::value_handler<Key, wire, std::decay_t<Fn>> { {}, std::forward<Fn>(handler) };
}

// Handler factory for an embedded sub-message at `Key`.  The wrapper
// constructs a nested `reader<InnerSchema>` over the payload and
// dispatches the inner handlers via its `parse()`.
//
// Two overloads:
//   on_submessage<K, S>(handlers...)         no init
//   on_submessage<K, S>(init, handlers...)   `init` runs after the
//                                            inner prescan, with inner
//                                            `meta<...>()` populated.
//
// The init callback is where you (a) inspect inner `meta<Key>()` for
// preallocation, and (b) emplace an entry in an output container and
// capture a pointer for field handlers to write through:
//
//   T *cur = nullptr;
//   on_submessage<K, S>(
//     [&](const reader<S> &) { cur = &vec.emplace_back(); return PPB_OK; },
//     on<F::x>([&](auto v) { cur->x = v; return PPB_OK; }))
//
// `K` must match a LEN-wire field in the outer schema (`bytes`,
// `utf8string`, `len`, `unpacked_bytes`, `message<K, S2>`, or
// `unpacked_message<K, S2>`).  Only for `message<K, S2>` /
// `unpacked_message<K, S2>` does the wrapper statically check
// `S == S2`; on other LEN fields the payload is parsed as `S` with
// no compile-time guarantee that the bytes are a serialized `S`.
//
// The wrapper owns the inner bounds; callers do not supply a `limit`.
// The outer call must enable submessage traversal with
// `limit::max_depth(N)` (N >= 1).  This check is at *runtime*: a
// missing `max_depth` returns `PPB_ERROR_DEPTH_EXCEEDED` only when an
// embedded field is first encountered, not at compile time.  Only
// the depth budget propagates to the inner reader (decremented by
// one): `max_fields` and the outer byte cap are not inherited.  The
// inner hard byte cap is the payload size.
//
// The `ppb_error`, if any, is propagated (modulo stickiness of the
// first write), but `unknown_field()` and `error_field()` aren't.
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
            if constexpr (detail::is_packed_range_v<std::decay_t<decltype(v)>>)
            {
                for (auto &&elem : v)
                    container->push_back(elem);
            }
            else
            {
                container->push_back(std::move(v));
            }

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
            if constexpr (detail::is_packed_range_v<std::decay_t<decltype(v)>>)
            {
                for (auto &&elem : v)
                    container->emplace_back(elem);
            }
            else
            {
                container->emplace_back(std::move(v));
            }

            return PPB_OK;
        });
}

// Catch-all (unknown-tag) handler: fires once per occurrence of any
// tag that matches a `ppb::unknown<wire>` entry in the schema.  The
// handler receives `const ppb_field &`; `field.v.ptr` points at the
// original tag byte, so callers who need the original field number
// can recover it via `ppb_decode_varint`.
//
// `wire` defaults to `wire_type::any` and matches every catch-all the
// schema registered; specify a concrete wire type to bind to one
// bucket.  Mixing per-wire `on_unknown` factories in a single
// `parse()` call is supported.  Return values fold first-write-wins
// into the reader's sticky error, like value handlers.
template <wire_type wire = wire_type::any, typename Fn>
[[nodiscard]] constexpr detail::unknown_handler<wire, std::decay_t<Fn>>
on_unknown(Fn &&handler)
{
    return detail::unknown_handler<wire, std::decay_t<Fn>> { {}, std::forward<Fn>(handler) };
}

// Stateful reader for a schema over a byte span.  Holds the
// unconsumed input, a sticky `ppb_error`, the optional
// `error_field()` / `unknown_field()` slots, and a per-field state
// array.  Trivially copyable; copies are independent snapshots.
//
// Lifecycle: construct with the input bytes, call `parse()` (or the
// lower-level `prescan`/`lexn`/`lex_all`), and either reuse with
// `reset_fields()` or construct a fresh reader for the next message.
// The sticky error/error_field/unknown_field state can only be
// cleared by constructing a fresh reader.
//
// There is no global state, so each `reader` is thread-compatible.
template <typename Schema> struct reader;
template <typename... Fs> struct reader<schema<Fs...>>
{
    using Schema = schema<Fs...>;

    // Default-constructs an empty reader (no input, no error).
   // `parse()`/`lexn()`/etc. on an empty reader are valid no-ops;
    // assign over it to start work.
    constexpr reader() noexcept = default;

    // Constructs a reader over `input`.  The reader does *not* own
    // the bytes; the buffer must outlive every call that consumes
    // from this reader and every decoded `string_view`/`span` handed
    // to a handler.  Spans larger than `PTRDIFF_MAX` are rejected by
    // setting the sticky error to `PPB_ERROR_TRUNCATED_DATA`.
    constexpr reader(std::span<const std::byte> input) noexcept
        : m_input(input)
    {
        if (m_input.size() > size_t(std::numeric_limits<ptrdiff_t>::max())) [[unlikely]]
            m_error = PPB_ERROR_TRUNCATED_DATA;
    }

    // (`ptr`, `length`) overload: builds the span for you.  Same
    // ownership and `PTRDIFF_MAX` rules apply.
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

    // Sticky error (initially `PPB_OK`).  Once non-OK, every
    // subsequent `prescan`/`lexn`/`parse`/`dispatch` short-circuits
    // and returns that error.  `reset_fields()` does *not* clear it;
    // construct a fresh reader to recover.
    [[nodiscard]] constexpr ppb_error error() const noexcept { return m_error; }

    // The *unconsumed* portion of the input.  After `lexn`/`parse`
    // the consumed prefix is no longer reachable from the reader.
    // Use `empty()` to detect end-of-input.
    [[nodiscard]] constexpr std::span<const std::byte> input() const noexcept { return m_input; }
    [[nodiscard]] constexpr size_t size() const noexcept { return m_input.size(); }
    [[nodiscard]] constexpr bool empty() const noexcept { return m_input.empty(); }

    // Encoded wire tag (`field_number * 8 + wire_type`) of *a* field
    // that triggered `field_semantics::error`, or `nullopt` if none
    // did.  When several error-semantics fields hit on the same scan,
    // which one is reported is unspecified; only its presence and
    // wire-tag value are.  Set by `parse()` only, not cleared by
    // `reset_fields()`.  The value always fits in `uint32_t`; the
    // return type matches `unknown_field()` for regularity.
    //
    // Special case: an `unknown<W, field_semantics::error>` catch-all
    // has no real field number, so the reported value is a sentinel
    // -- the catch-all's `field_number == uint64_t(-1)` truncated to
    // 32 bits and shifted, i.e. `0xFFFFFFF8u | uint32_t(W)`.  These
    // sentinels collide with a hypothetical schema field at
    // `field_number == 2**29 - 1`; in practice you only see them when
    // an `unknown<W, error>` is wired into the schema.
    [[nodiscard]] constexpr std::optional<uint64_t> error_field() const noexcept
    {
        if (m_error_field != 0)
            return m_error_field;

        return {};
    }

    // Encoded wire tag (`field_number * 8 + wire_type`) of *an*
    // unknown field that hit a `ppb::unknown<wire>` catch-all, or
    // `nullopt` if none did.  Which occurrence is reported (across
    // multiple unknown tags within one wire bucket, or across the
    // four wire-type catch-alls) is unspecified; only the presence
    // of an unknown field and a valid wire-tag value are.  Sticky
    // and **soft**: once set, later calls leave it alone, and it
    // does *not* set the reader's error or abort dispatch.
    // Populated by `parse()` and `prescan()`; standalone `lexn()`
    // leaves it alone.  Not cleared by `reset_fields()`.
    [[nodiscard]] constexpr std::optional<uint64_t> unknown_field() const noexcept
    {
        if (m_unknown_field != 0)
            return m_unknown_field;

        return {};
    }

    // Zeroes the per-field state in preparation for another
    // prescan/parse call.  Does *not* clear `error()`,
    // `error_field()`, or `unknown_field()`; a reader that has
    // already failed cannot be revived this way.  Useful for
    // re-running prescan/lexn on the same bytes with different
    // handlers; for stream processing, construct a fresh reader per
    // message.
    constexpr void reset_fields() noexcept { m_fields = {}; }

    // High-level driver: prescan + optional lexn pass + dispatch.
    // **Consumes** from the input span on success: after `parse()`
    // returns `PPB_OK`, `input()` reflects only the unconsumed suffix.
    // Five equivalent call shapes (all forward to the last):
    //
    //   parse(handlers...)
    //   parse(limit, handlers...)
    //   parse(init, handlers...)
    //   parse(limit, init, handlers...)
    //   parse(init, limit, handlers...)   // canonical form
    //
    // `std::nullopt` is also accepted as `init`, and `{}` as `limit`.
    //
    // After prescan, `init(std::as_const(*this))` runs with prescan
    // metadata available via `meta<Key>()` and returns a `enum
    // ppb_error`.  When `init` returns `PPB_OK` (`parse` immediately
    // returns with the error otherwise), handlers are dispatched
    // either once (prescan fast path) or per batch via `ppb_lexn`,
    // depending on the schema's `field_semantics`.
    //
    // `handlers...` may be empty when `init` is supplied:
    // `parse(init)` runs prescan + `init` (with `meta<>()` populated)
    // and returns without dispatching any field handlers -- useful
    // for validation or sizing without decoding.
    //
    // Handler call order across a batch is *not* specified: don't
    // rely on schema order, wire order, or argument order.
    //
    // Exceptional cases:
    //   - A `field_semantics::error` field seen on the wire makes
    //     parse() return `PPB_ERROR_CORRUPT_TAG`, sets
    //     `error_field()`, and skips both `init` and handlers.
    //   - Non-OK `init` return is recorded as the sticky error; the
    //     input span is *not* advanced.
    //   - Empty input (or prescan reporting zero bytes) returns
    //     `PPB_OK` without invoking `init` or any handler.  Absent
    //     `proto3_zero_default` fields do not fire in that case.
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

    // Tuple-accepting variant: reuses a pre-built handler tuple
    // without `std::apply`.  Taken by mutable reference so stateful
    // handlers can mutate themselves across batches.
    template <typename Init, typename... Hs>
        requires(detail::is_handler_v<Hs> && ...)
    [[nodiscard]] ppb_error parse_tuple(Init &&init, limit bounds, std::tuple<Hs...> &handlers);

    // Runs `ppb_prescan` and, if handlers are passed, dispatches them
    // once with the aggregated values (last-occurrence for scalars,
    // last-seen payload for LEN).  For non-empty inputs, absent
    // `proto3_zero_default` fields dispatch with their zero value,
    // matching `parse()`'s fast path.  Returns bytes prescanned (>=
    // 0) on success, or a negative `ppb_error`.  Does *not* advance
    // the input span.  Handler errors fold into the sticky error and
    // are reflected in the return value.
    template <typename... Hs> [[nodiscard]] ptrdiff_t prescan(limit bounds = {}, Hs &&...handlers);

    // Prescan-aggregated `ppb_field_meta` for `key`.  Omitting `wire`
    // merges metadata across all schema entries sharing `key`.
    // Compile error when nothing matches.  Zero-initialized before
    // the first `prescan`/`parse`.
    //
    // Only meaningful after `prescan()` or `parse()` (or in the
    // `init` callback of `parse()`).
    template <typename Schema::Key key, wire_type wire = wire_type::any>
    constexpr ppb_field_meta meta() const noexcept;

    // Runs `ppb_lexn` once: decodes one batch of strictly
    // monotonically-increasing fields and dispatches matching
    // handlers.  Advances the input span past the consumed bytes
    // unconditionally (including on non-fatal limit errors).
    //
    // Unlike `parse()`, `lexn()` does *not* interpret
    // `field_semantics`: every occurrence dispatches, and `error`
    // semantics are not enforced.  Filter on the handler side or use
    // `parse()` when you need that policy logic.
    //
    // Call in a loop until `empty()` or `error() != PPB_OK`; or
    // prefer `lex_all()` for that common pattern.
    //
    // *Consumes* from the input span!
    template <typename... Hs> [[nodiscard]] ppb_error lexn(limit bounds = {}, Hs &&...handlers)
    {
        std::tuple<std::decay_t<Hs>...> tup(std::forward<Hs>(handlers)...);
        return lexn_tuple(bounds, tup);
    }

    template <typename... Hs> [[nodiscard]] ppb_error lexn_tuple(limit bounds, std::tuple<Hs...> &handlers);

    // Drives `lexn()` in a loop until the input is empty or an error
    // is set.  `bounds` applies *per batch*, not cumulatively.
    // Dispatch and sticky-error contract are identical to `lexn()`.
    // Unlike `parse()`, no upfront prescan: outer `meta<>()` and
    // `unknown_field()` stay zero, and encoding errors surface per
    // batch.  Use `parse()` (or `prescan()` then `lex_all()`) when
    // you need either.
    //
    // Immediately bails if the reader's sticky error is set.
    template <typename... Hs> [[nodiscard]] ppb_error lex_all(Hs &&...handlers)
    {
        return lex_all(limit {}, std::forward<Hs>(handlers)...);
    }

    // Returns immediately with the sticky error (or PPB_OK if none)
    // when bounds forbids any progress (byte or field limit of 0).
    template <typename... Hs> [[nodiscard]] ppb_error lex_all(limit bounds, Hs &&...handlers)
    {
        std::tuple<std::decay_t<Hs>...> tup(std::forward<Hs>(handlers)...);
        return lex_all_tuple(bounds, tup);
    }

    // Returns immediately with the sticky error (or PPB_OK if none)
    // when bounds forbids any progress (byte or field limit of 0).
    template <typename... Hs> [[nodiscard]] ppb_error lex_all_tuple(limit bounds, std::tuple<Hs...> &tup);

    // Re-dispatches handlers against the current per-field state
    // without touching the input span.  Mostly useful for tests.
    // Unlike `parse()`'s fast path and `prescan(handlers...)`,
    // `dispatch()` does *not* synthesize absent
    // `proto3_zero_default` fields (only fields with `field.v.ptr !=
    // nullptr` fire); call `dispatch_tuple(tup, /*run_zero_defaults=*/
    // true)` directly to opt in.  Prefer `dispatch_tuple` from
    // generic code that already has a tuple in hand, or when
    // you want to apply a `limit`.
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
    // Zero is a safe "unset" sentinel: a zero encoded tag is rejected
    // at compile time for schema fields and at runtime as invalid wire
    // encoding for unknown fields.  First non-zero write wins.
    //
    // `m_error_field` fits in uint32_t because schema-field tags are
    // bounded to < 2**29 at compile time; `m_unknown_field` is wider
    // because unknown wire tags are not bounded.
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

    // we already bailed out if num_bytes <= 0 (not just on error),
    // so we know we have *some* bytes to read here.
    if (m_error != PPB_OK) [[unlikely]]
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

        // Defensive: prescan validated the wire encoding and the C
        // library guarantees forward progress within the hard limit,
        // so neither branch should fire.  Bail anyway in case the input
        // changed between prescan and lexn.
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

    static_assert(range.count > 0, "Key/wire combination not found in schema");
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
                // Absent proto3_zero_default fields have field.v.ptr == 0;
                // widening the bound to UINTPTR_MAX lets them pass the
                // range check below so the handler dispatches with the
                // zero value.
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
