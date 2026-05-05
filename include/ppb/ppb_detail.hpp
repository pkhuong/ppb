// Never include this file directly, it should only be included by ppb.hpp.

namespace ppb
{

struct field_generic_base
{
};

template <auto K, wire_type type> struct field_base : public field_generic_base
{
    static_assert(static_cast<uint64_t>(K) - 1 < (uint64_t(1) << 29) - 1,
        "field tag key must be convertible to uint64_t and must fit in [1, 2**29 - 1].");
    static_assert(type == wire_type::varint || type == wire_type::i64 || type == wire_type::len ||
            type == wire_type::i32,
        "field wire type must be one of varint, i64, len, or i32");

    using Key = decltype(K);

    static constexpr Key tag() { return K; }
    static constexpr wire_type wire() { return type; }

    static constexpr ppb_encoded_tag encoded_tag()
    {
        return ppb_encoded_tag { .bits = PPB_TAG_BITS(static_cast<uint64_t>(K), uint64_t(type)) };
    }
};

template <auto K> struct varint : public field_base<K, wire_type::varint>
{
};

template <auto K> struct i64 : public field_base<K, wire_type::i64>
{
};

template <auto K> struct len : public field_base<K, wire_type::len>
{
};

template <auto K> struct i32 : public field_base<K, wire_type::i32>
{
};

namespace detail
{
}  // namespace detail
}  // namespace ppb
