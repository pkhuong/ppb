#pragma once

#include "ppb.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>

static_assert(std::endian::native == std::endian::little, "ppb.hpp currently requires a little-endian host");

namespace ppb
{

enum class wire_type : uint8_t
{
    varint = PPB_WIRE_VARINT,
    i64 = PPB_WIRE_I64,
    len = PPB_WIRE_LEN,
    i32 = PPB_WIRE_I32,
};

// PPB field descriptors all inherit from `field_base<K, T>`, which is-a
// `field_generic_base`.
struct field_generic_base;
template <auto K, wire_type type> struct field_base;

// A varint-encoded field
template <auto K> struct varint;
// An i64-encoded field
template <auto K> struct i64;
// A length-prefixed field
template <auto K> struct len;
// An i32-encoded field
template <auto K> struct i32;

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

    // Runs prescan (subject to `bounds`) on the input span.
    [[nodiscard]] ptrdiff_t prescan(limit bounds = {})
    {
        if (m_error != PPB_OK) [[unlikely]]
            return m_error;

        ptrdiff_t ret = ppb_prescan_impl(make_ppb_buf(), m_fields.size(), Schema::s_encoded_tags.data(),
            m_fields.data(), bounds.fields(), bounds.bytes(), bounds.error_on_bytes());

        if (ret < 0) [[unlikely]]
        {
            m_error = ppb_error(ret);
            return m_error;
        }

        return ret;
    }

private:
    constexpr ppb_buf make_ppb_buf() const noexcept
    {
        return ppb_buf {
            .buf = m_input.data(),
            .size = m_input.size(),
        };
    }

    std::span<const std::byte> m_input;
    ppb_error m_error = PPB_OK;
    std::array<ppb_field, Schema::num_fields()> m_fields = {};
};
}  // namespace ppb
