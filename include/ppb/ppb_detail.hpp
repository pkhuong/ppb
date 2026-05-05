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

template <typename...> struct key_of
{
    using type = int;
};
template <typename First, typename... Rest> struct key_of<First, Rest...>
{
    using type = typename First::Key;
};

template <typename... Fs> struct schema_impl
{
    using Key = typename key_of<Fs...>::type;

    static constexpr size_t num_fields() { return sizeof...(Fs); }

    static consteval bool fields_non_empty() { return num_fields() > 0; }

    static consteval bool fields_are_fields() { return (std::is_base_of_v<field_generic_base, Fs> && ...); }

    static consteval bool fields_have_same_key_type()
    {
        return (std::is_same_v<typename Fs::Key, Key> && ...);
    }

    static consteval std::array<ppb_encoded_tag, sizeof...(Fs)> encoded_tags()
    {
        return { Fs::encoded_tag()... };
    }

    static consteval bool tags_are_in_order()
    {
        if constexpr (num_fields() <= 1)
            return true;

        constexpr auto tags = encoded_tags();
        for (size_t i = 1; i < tags.size(); i++)
        {
            if (tags[i - 1].bits >= tags[i].bits)
                return false;
        }
        return true;
    }
};

}  // namespace detail
}  // namespace ppb
