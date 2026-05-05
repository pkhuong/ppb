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

    struct field_range
    {
        size_t begin = 0;
        size_t count = 0;
    };

    // returns a field_range for the tags that match `k`.
    static consteval field_range find_field_range(Key k, wire_type wire)
    {
        uint64_t wanted = PPB_TAG_BITS(static_cast<uint64_t>(k), 0) / 8;  // drop wire type
        const auto haystack = encoded_tags();

        field_range ret;

        for (size_t idx = 0; idx < haystack.size(); idx++)
        {
            uint64_t got = haystack[idx].bits / 8;
            uint64_t got_wire = haystack[idx].bits % 8;

            if (got > wanted)
                break;

            if (got != wanted)
                continue;

            if (wire != wire_type::any && got_wire != uint64_t(wire))
                continue;

            if (ret.count == 0)
            {
                ret.begin = idx;
            }

            ret.count++;
        }

        return ret;
    }
};

constexpr void
merge_meta(ppb_field_meta &acc, ppb_field_meta upd) noexcept
{
    if (acc.num_occurrences == 0)
    {
        acc = upd;
        return;
    }

    if (upd.num_occurrences == 0)
        return;

    acc.num_occurrences += upd.num_occurrences;
    acc.lost_distinct_u64 = true;
    acc.total_bytes += upd.total_bytes;
    acc.min_nonzero_bytes = (acc.min_nonzero_bytes - 1 > upd.min_nonzero_bytes - 1) ? upd.min_nonzero_bytes :
                                                                                      acc.min_nonzero_bytes;
    acc.max_bytes = (acc.max_bytes < upd.max_bytes) ? upd.max_bytes : acc.max_bytes;
}

template <typename Fn>
[[gnu::always_inline]] constexpr bool
dispatch_16(size_t begin, Fn &&handler)
{
#define PPB_DETAIL_DISPATCH(N)                                \
    case (N):                                                 \
        if (!handler(std::integral_constant<size_t, (N)> {})) \
            return false;                                     \
        [[fallthrough]]

    switch (begin)
    {
        PPB_DETAIL_DISPATCH(0);
        PPB_DETAIL_DISPATCH(1);
        PPB_DETAIL_DISPATCH(2);
        PPB_DETAIL_DISPATCH(3);
        PPB_DETAIL_DISPATCH(4);
        PPB_DETAIL_DISPATCH(5);
        PPB_DETAIL_DISPATCH(6);
        PPB_DETAIL_DISPATCH(7);
        PPB_DETAIL_DISPATCH(8);
        PPB_DETAIL_DISPATCH(9);
        PPB_DETAIL_DISPATCH(10);
        PPB_DETAIL_DISPATCH(11);
        PPB_DETAIL_DISPATCH(12);
        PPB_DETAIL_DISPATCH(13);
        PPB_DETAIL_DISPATCH(14);
        PPB_DETAIL_DISPATCH(15);

    default:
        break;
    }

#undef PPB_DETAIL_DISPATCH

    return true;
}

template <size_t limit, typename Fn>
[[gnu::always_inline]] constexpr bool
dispatch(size_t begin, size_t end, Fn &&handler)
{
    constexpr size_t num_blocks = (limit + 15) / 16;

    auto handle_block = [&]<size_t base>(std::integral_constant<size_t, base>) -> bool
    {
        constexpr size_t local_limit = limit < base + 16 ? limit : base + 16;
        // `base >= end` short-circuits the outer fold: every later
        // block has a strictly larger base, so they're all past `end`.
        if (base >= end) [[unlikely]]
            return false;

        if (begin >= local_limit) [[unlikely]]
            return true;

        size_t local = (begin > base) ? begin - base : 0;
        auto wrapped = [&handler, end]<size_t I>(std::integral_constant<size_t, I>) -> bool
        {
            if constexpr (base + I < limit)
            {
                // Runtime upper bound: stop once we reach `end`.
                if (base + I >= end) [[unlikely]]
                    return false;

                bool ret = handler(std::integral_constant<size_t, base + I> {});
                if constexpr (base + I + 1 == limit)
                {
                    // Always stop after the last static case.
                    return false;
                }
                else
                {
                    return ret;
                }
            }
            else
            {
                return false;
            }
        };

        return dispatch_16(local, wrapped);
    };

    bool ok = true;
    [&]<size_t... Bs>(std::index_sequence<Bs...>)
    { ((ok = ok && handle_block(std::integral_constant<size_t, Bs * 16> {})), ...); }(
        std::make_index_sequence<num_blocks> {});

    return ok;
}
}  // namespace detail
}  // namespace ppb
