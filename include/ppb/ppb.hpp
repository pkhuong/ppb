#pragma once

#include "ppb.h"

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

namespace ppb
{

enum class wire_type : uint8_t
{
    varint = PPB_WIRE_VARINT,
    i64 = PPB_WIRE_I64,
    len = PPB_WIRE_LEN,
    i32 = PPB_WIRE_I32,

    any = 255,  // sentinel value for meta<>
};

// field semantics for `reader::parse`.
enum class field_semantics : int8_t
{
    always_lexn = -1,  // always force lexn, even for a single occurrence
    repeated = 0,  // we want to see everything
    singular = 1,  // it's ok to do LWW if all the squashed values are equivalent
    last_write_wins = 2,  // we only want to see the last value (regular proto semantics)
    error = 127,  // just report a PPB_ERROR_CORRUPT_TAG if we see this
};

// PPB field descriptors all inherit from `field_base<K, T>`, which is-a
// `field_generic_base`.
struct field_generic_base;
template <auto K, wire_type type> struct field_base;

// A varint-encoded field
template <auto K, field_semantics sem = field_semantics::repeated> struct varint;
// An i64-encoded field
template <auto K, field_semantics sem = field_semantics::repeated> struct i64;
// A length-prefixed field
template <auto K, field_semantics sem = field_semantics::repeated> struct len;
// An i32-encoded field
template <auto K, field_semantics sem = field_semantics::repeated> struct i32;

// Varint-backed scalar field types
template <auto K, field_semantics sem = field_semantics::singular> struct int32;
template <auto K, field_semantics sem = field_semantics::singular> struct int64;
template <auto K, field_semantics sem = field_semantics::singular> struct sint32;
template <auto K, field_semantics sem = field_semantics::singular> struct sint64;
template <auto K, field_semantics sem = field_semantics::singular> struct uint32;
template <auto K, field_semantics sem = field_semantics::singular> struct uint64;
template <auto K, field_semantics sem = field_semantics::singular> struct boolean;
template <auto K, typename Enum, field_semantics sem = field_semantics::singular,
    typename UnderlyingType = std::underlying_type_t<Enum>>
struct enumerated;

// I32-backed scalar field types
template <auto K, field_semantics sem = field_semantics::singular> struct fixed32;
template <auto K, field_semantics sem = field_semantics::singular> struct sfixed32;
template <auto K, field_semantics sem = field_semantics::singular> struct f32;

// I64-backed scalar field types
template <auto K, field_semantics sem = field_semantics::singular> struct fixed64;
template <auto K, field_semantics sem = field_semantics::singular> struct sfixed64;
template <auto K, field_semantics sem = field_semantics::singular> struct f64;

// LEN-backed scalar field types
template <auto K, field_semantics sem = field_semantics::singular> struct utf8string;
template <auto K, typename Element = std::byte, field_semantics sem = field_semantics::singular> struct bytes;

/*
 * Wraps a fixed-width value stored as little-endian, unaligned bytes
 * (the protobuf wire format for fixed32/sfixed32/fixed64/sfixed64/float/double).
 * The conversion to `T` returns the host-order value, byte-swapping if
 * the host is big-endian.
 */
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
template <auto K, typename Enum, typename UnderlyingType = std::underlying_type_t<Enum>>
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
template <auto K, typename Element> using unpacked_bytes = bytes<K, Element, field_semantics::repeated>;

}  // namespace ppb

// clang-format off
#include "ppb_detail.hpp"
// clang-format on

namespace ppb
{

// A schema is a type list of field types.
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

// Limit on the number of fields processed at a time, and hard/soft
// limit on the number of bytes lexed.
struct limit
{
public:
    static constexpr limit max_fields(size_t max) noexcept { return limit().with_max_fields(max); }

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

    constexpr limit with_max_bytes(size_t max_bytes) const noexcept
    {
        limit ret = *this;
        ret.m_max_bytes = max_bytes;
        return ret;
    }

    constexpr size_t fields() const noexcept { return m_max_fields; }
    constexpr size_t bytes() const noexcept { return m_max_bytes; }
    constexpr ppb_error error_on_bytes() const noexcept { return m_error_on_bytes; }

private:
    size_t m_max_fields = std::numeric_limits<size_t>::max();
    size_t m_max_bytes = std::numeric_limits<size_t>::max();
    ppb_error m_error_on_bytes = PPB_OK;
};

// Stateful reader for a schema / span.
template <typename Schema> struct reader;
template <typename... Fs> struct reader<schema<Fs...>>
{
    using Schema = schema<Fs...>;

    constexpr reader() noexcept = default;
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

    // The `ppb_error` for a reader starts as PPB_OK, and remains sticky afterward.
    //
    // Once the error is non-zero, we stop trying to prescan or lex bytes.
    [[nodiscard]] constexpr ppb_error error() const noexcept { return m_error; }
    [[nodiscard]] constexpr std::span<const std::byte> input() const noexcept { return m_input; }
    [[nodiscard]] constexpr size_t size() const noexcept { return m_input.size(); }
    [[nodiscard]] constexpr bool empty() const noexcept { return m_input.empty(); }
    [[nodiscard]] constexpr std::optional<uint32_t> error_field() const noexcept { return m_error_field; }

    // Zero-initialized the fields array. This does not reset `error`
    // or `error_field`.  Create a fresh reader from `reader::span()`
    // to reset the sticky error state.
    constexpr void reset_fields() noexcept { m_fields = {}; }

    // Runs `prescan` with `bounds`, then invokes `init` with a const
    // reference to `this` (use `reader::meta()` to gather metadata
    // and preallocate the destination), and finally runs `lexn` on
    // the input read by `prescan` (disregarding bounds), while
    // invoking the handlers.
    //
    // Exits early when prescan reads 0 bytes.
    //
    // *Consumes* from the input span once `init` returns success!
    // Call `reset_fields` after `parse` to prepare for the next message.
    template <typename Init, typename... Hs>
    [[nodiscard]] ppb_error parse(Init &&init, limit bounds = {}, Hs &&...handlers)
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

        // Check field semantics: error fields that appeared on the wire
        // are an immediate failure.  Also determine whether any field
        // forces a lexn pass.
        bool has_error = false;
        bool need_lexn = false;
        [&]<size_t... Is>(std::index_sequence<Is...>)
        {
            auto scan = [&]<size_t I>(std::integral_constant<size_t, I>)
            {
                using Field = decltype(Schema::template field<I>());
                const field_semantics semantics = Field::semantics();
                const auto &meta = m_fields[I].m;

                if constexpr (semantics == field_semantics::error)
                {
                    if (meta.num_occurrences > 0)
                    {
                        has_error = true;
                        m_error_field = static_cast<uint32_t>(Field::tag()) * 8 +
                            static_cast<uint32_t>(Field::wire());
                    }

                    return;
                }

                if constexpr (semantics == field_semantics::always_lexn)
                {
                    need_lexn |= meta.num_occurrences > 0;
                    return;
                }

                if (meta.num_occurrences <= 1)
                    return;  // single-occurrence never forces lexn

                if constexpr (semantics == field_semantics::repeated)
                {
                    need_lexn = true;
                    return;
                }

                if constexpr (semantics == field_semantics::singular)
                {
                    if (Field::wire() == wire_type::len || meta.lost_distinct_u64)
                        need_lexn = true;
                }

                // last_write_wins: never forces lexn
            };
            (scan(std::integral_constant<size_t, Is> {}), ...);
        }(std::make_index_sequence<Schema::num_fields()> {});

        if (has_error) [[unlikely]]
        {
            m_error = PPB_ERROR_CORRUPT_TAG;
            return m_error;
        }

        std::tuple<std::decay_t<Hs>...> tup(std::forward<Hs>(handlers)...);

        m_error = init(std::as_const(*this));
        if (m_error != PPB_OK) [[unlikely]]
            return m_error;

        if (!need_lexn) [[likely]]
        {
            run_handlers(tup, reinterpret_cast<uintptr_t>(m_input.data()), size_t(num_bytes));
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
            // empty range happens only on error or empty input
            if (ret.status != PPB_OK || range_size == 0) [[unlikely]]
            {
                m_error = ret.status;
                break;
            }

            run_handlers(tup, reinterpret_cast<uintptr_t>(base), range_size - 1, ret.first_field,
                ret.first_field + ret.field_range);
            if (m_error != PPB_OK) [[unlikely]]
                break;
        }

        m_input = std::span(reinterpret_cast<const std::byte *>(buf.buf), buf.size);
        return m_error;
    }

    // Runs prescan (subject to `bounds`) on the input span.  When any `on()`
    // handlers are passed, dispatches to them after a successful prescan.
    // Each handler that matches a field we found on the wire is invoked with
    // the corresponding `struct ppb_field`, and must return a `ppb_error`
    // (errors are accumulated into the reader's error, with first-write-wins,
    // but all handlers are invoked).
    //
    // Returns a ppb_error (negative value) on error, or a non-negative
    // number of bytes read by the `prescan` call.
    template <typename... Hs> [[nodiscard]] ptrdiff_t prescan(limit bounds = {}, Hs &&...handlers)
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

        ppb_error err = dispatch(std::forward<Hs>(handlers)...);
        return int(err) < 0 ? ptrdiff_t(err) : ret;
    }

    // Returns the metadata associated with `key`.
    //
    // Errors at compile-time if there is no such key (or key/wire
    // type pair) in the schema.
    //
    // By default, merges the metadata for all wire types associated
    // with `key`; specify a `wire` type to avoid merging and return
    // only the exact hit.
    template <Schema::Key key, wire_type wire = wire_type::any> constexpr ppb_field_meta meta() const noexcept
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

    // Runs `lexn` *once* subject to bounds on the input span.  When
    // any `on()` handlers are passed, dispatches to them each
    // successful `lexn`.  Each handler that matches a field we found
    // on the wire is invoked with the corresponding `struct
    // ppb_field`, and must return a `ppb_error` (errors are
    // accumulated into the reader's error, with first-write-wins, but
    // all handlers are invoked).
    //
    // *Consumes* from the input span!
    template <typename... Hs> [[nodiscard]] ppb_error lexn(limit bounds = {}, Hs &&...handlers)
    {
        if (m_error != PPB_OK) [[unlikely]]
            return m_error;

        std::tuple<std::decay_t<Hs>...> tup(std::forward<Hs>(handlers)...);

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

        // range_size == 0 should only happen on empty input (or maybe on error).
        if (m_error != PPB_OK || range_size == 0) [[unlikely]]
        {
            return m_error;
        }

        run_handlers(tup, base, range_size - 1, ret.first_field, ret.first_field + ret.field_range);
        return m_error;
    }

    // Runs `on()` handlers on entries for which we found a value.
    template <typename... Hs> ppb_error dispatch(Hs &&...handlers)
    {
        std::tuple<std::decay_t<Hs>...> tup(std::forward<Hs>(handlers)...);
        return dispatch_tuple(tup);
    }

    template <typename... Hs> ppb_error dispatch_tuple(std::tuple<Hs...> &handlers)
    {
        if constexpr (sizeof...(Hs) == 0)
            return m_error;

        if (m_error != PPB_OK) [[unlikely]]
            return m_error;

        run_handlers(handlers, 1, std::numeric_limits<uintptr_t>::max() - 1);
        return m_error;
    }

private:
    constexpr ppb_buf make_ppb_buf() const noexcept
    {
        return ppb_buf {
            .buf = m_input.data(),
            .size = m_input.size(),
        };
    }

    template <typename... Hs>
    [[gnu::noinline]] void run_handlers(std::tuple<Hs...> &handlers, uintptr_t lower_bound,
        uintptr_t range_size_inclusive, size_t begin = 0, size_t end = Schema::num_fields())
    {
        detail::dispatch<Schema::num_fields()>(begin, end,
            [&]<size_t I>(std::integral_constant<size_t, I>)
            {
                run_handler_for_idx<I>(handlers, lower_bound, range_size_inclusive);
                return true;
            });
    }

    template <size_t idx, typename... Hs>
    [[gnu::always_inline]] void run_handler_for_idx(std::tuple<Hs...> &handlers, uintptr_t lower_bound,
        uintptr_t range_size_inclusive)
    {
        const ppb_field &field = m_fields[idx];

        // Find a handler for the schema field.
        using Field = decltype(Schema::template field<idx>());
        constexpr std::optional<size_t> handler_idx = detail::find_value_handler<Field::tag(), Field::wire(),
            decltype(Field::extract_value(field, &m_error)), Hs...>();
        if constexpr (handler_idx.has_value())
        {
            auto field_addr = reinterpret_cast<uintptr_t>(field.v.ptr);

            if (field_addr - lower_bound <= range_size_inclusive) [[likely]]
            {
                enum ppb_error result = std::get<handler_idx.value()>(handlers).handler(
                    Field::extract_value(field, &m_error));
                if (result != PPB_OK) [[unlikely]]
                {
                    m_error = (m_error == PPB_OK) ? result : m_error;
                }
            }
        }
    }

    std::span<const std::byte> m_input;
    ppb_error m_error = PPB_OK;
    std::optional<uint32_t> m_error_field;
    std::array<ppb_field, Schema::num_fields()> m_fields = {};
};

// Wraps a function as a field handler for the Key, and potentially
// for the wire type.
//
// The `reader`'s `parse` method (as well as the lower level
// `prescan`, `lexn`, and `dispatch` methods) invokes the handler when
// it matches a decoded field.
//
// Each handler is invoked with a `const ppb_field &`, and returns a
// `enum ppb_error`; non-zero errors are piped back to the reader and
// stop the lexing loop, if necessary (handlers for the same lexn
// batch are still invoked).
template <auto Key, wire_type wire = wire_type::any, typename Fn>
[[nodiscard]] constexpr detail::value_handler<Key, wire, std::decay_t<Fn>>
on(Fn &&handler)
{
    return detail::value_handler<Key, wire, std::decay_t<Fn>> { std::forward<Fn>(handler) };
}

}  // namespace ppb
