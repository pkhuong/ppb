// Private implementation details for the PPB C++ wrapper.
//
// Included exactly once by <ppb/ppb.hpp>, after the public forward
// declarations but before the `ppb::schema`/`ppb::reader` definitions.
// Do not include directly; nothing here is a stable API.
namespace ppb
{

// Forward declarations for `submessage_handler::invoke`, which is
// defined out-of-line in ppb.hpp once `limit` and `reader` exist.
struct limit;
template <typename Schema> struct reader;

// Base for every field descriptor.  Typed fields inherit via
// `field_base<K, wire_type>` (which adds the field-number range and
// wire-type static_asserts plus `tag()`/`wire()`/`encoded_tag()`).
// Catch-alls (`ppb::unknown<wire>`) inherit from this directly to
// bypass the key-range check and skip the same-Key-type assertion.
//
// Defaults here are shadowed by derived classes as needed.
struct field_generic_base
{
    static constexpr const ppb_field &extract_value(const ppb_field &field, ppb_error *) { return field; }
    static constexpr field_semantics semantics() { return field_semantics::always_lexn; }

    // Catch-all marker; shadowed by `ppb::unknown<>` to return true.
    // Used by the reader to identify catch-all entries during dispatch
    // and the post-prescan field walk, without a separate trait.
    static constexpr bool is_unknown() { return false; }

    [[gnu::always_inline]] static constexpr void maybe_flag_unknown_field(const ppb_field &,
        std::span<const std::byte>, uint64_t *)
    {
    }
};

// Core of the prescan-before-lexn decision.  Given a field's
// compile-time tag, wire type, semantics, and whether a handler is
// registered, decide whether the field forces a lexn pass and/or
// triggers a `field_semantics::error` failure.  Called by
// `reader::parse()` via each field type's `classify_field_before_lexn`
// thunk (which just plugs its own constants in).
//
// Decision table (has_handler = true, multiple occurrences):
//
//   semantics            forces lexn?   notes
//   ---------           -------------   -----
//   error                no             returns error tag immediately
//   always_lexn          yes            even on single occurrence
//   repeated             yes            single occurrence stays on fast path
//   singular             yes*           * only when LEN or lost_distinct_u64
//   last_write_wins      no
//   proto3_zero_default  no
//
// Without a handler, nothing forces lexn.
template <uint32_t tag, wire_type wire, field_semantics sem, bool has_handler>
[[gnu::always_inline]] static constexpr std::optional<uint32_t>
classify_field_before_lexn_impl(const ppb_field_meta &meta, bool *need_lexn)
{
    if constexpr (sem == field_semantics::error)
    {
        if (meta.num_occurrences > 0) [[unlikely]]
            return 8 * tag + static_cast<uint32_t>(wire);

        return {};
    }

    if constexpr (!has_handler)
        return {};

    if constexpr (sem == field_semantics::always_lexn)
    {
        *need_lexn |= meta.num_occurrences > 0;
        return {};
    }

    // Unless we force always_lexn, <= 1 occurrences can be handled
    // with prescan.
    if (meta.num_occurrences <= 1)
        return {};

    if constexpr (sem == field_semantics::repeated)
    {
        *need_lexn = true;
    }
    else if constexpr (sem == field_semantics::singular)
    {
        // lost_distinct_u64 isn't populated for LEN wire type; assume the worst.
        if (wire == wire_type::len || meta.lost_distinct_u64)
            *need_lexn = true;
    }
    // last_write_wins / proto3_zero_default: never forces lexn

    return {};
}

// Common base for fields with a real (in-range) field number.
// Static_asserts:
//   - K convertible to uint64_t
//   - 1 <= K <= 2**29 - 1 (the protobuf field-number range)
//   - `type` is one of `varint`, `i64`, `len`, `i32`
//
// Catch-all entries (`ppb::unknown<wire>`) bypass these checks by
// deriving from `field_generic_base` directly.
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

template <auto K, field_semantics sem> struct varint : public field_base<K, wire_type::varint>
{
    static constexpr field_semantics semantics() { return sem; }

    template <bool has_handler>
    [[gnu::always_inline]] static constexpr std::optional<uint32_t>
    classify_field_before_lexn(const ppb_field_meta &meta, bool *need_lexn)
    {
        return classify_field_before_lexn_impl<uint32_t(varint::tag()), varint::wire(), varint::semantics(),
            has_handler>(meta, need_lexn);
    }
};

template <auto K, field_semantics sem> struct i64 : public field_base<K, wire_type::i64>
{
    static constexpr field_semantics semantics() { return sem; }

    template <bool has_handler>
    [[gnu::always_inline]] static constexpr std::optional<uint32_t>
    classify_field_before_lexn(const ppb_field_meta &meta, bool *need_lexn)
    {
        return classify_field_before_lexn_impl<uint32_t(i64::tag()), i64::wire(), i64::semantics(),
            has_handler>(meta, need_lexn);
    }
};

template <auto K, field_semantics sem> struct len : public field_base<K, wire_type::len>
{
    static constexpr field_semantics semantics() { return sem; }

    template <bool has_handler>
    [[gnu::always_inline]] static constexpr std::optional<uint32_t>
    classify_field_before_lexn(const ppb_field_meta &meta, bool *need_lexn)
    {
        return classify_field_before_lexn_impl<uint32_t(len::tag()), len::wire(), len::semantics(),
            has_handler>(meta, need_lexn);
    }
};

template <auto K, field_semantics sem> struct i32 : public field_base<K, wire_type::i32>
{
    static constexpr field_semantics semantics() { return sem; }

    template <bool has_handler>
    [[gnu::always_inline]] static constexpr std::optional<uint32_t>
    classify_field_before_lexn(const ppb_field_meta &meta, bool *need_lexn)
    {
        return classify_field_before_lexn_impl<uint32_t(i32::tag()), i32::wire(), i32::semantics(),
            has_handler>(meta, need_lexn);
    }
};

// Catch-all descriptor mapped onto the C lexer's `PPB_TAG(-1, wire)`
// entry: matches any tag of the given wire type that the schema
// doesn't otherwise list.  Derives from `field_generic_base` directly
// to bypass the `[1, 2**29 - 1]` key-range check, and intentionally
// exposes no `Key` typedef so the schema's same-Key-type assertion
// skips it.
//
// Defaults to `field_semantics::repeated` so any registered
// `on_unknown` handler sees every matching occurrence.  Without such
// a handler, we don't force a lexn pass solely for unknown fields.
template <wire_type type, field_semantics sem> struct unknown : public field_generic_base
{
    static_assert(type == wire_type::varint || type == wire_type::i64 || type == wire_type::len ||
            type == wire_type::i32,
        "ppb::unknown wire type must be one of varint, i64, len, or i32");

    static constexpr uint64_t tag() { return UINT64_MAX; }
    static constexpr wire_type wire() { return type; }

    static constexpr ppb_encoded_tag encoded_tag()
    {
        return ppb_encoded_tag { .bits = PPB_TAG_BITS(UINT64_MAX, uint64_t(type)) };
    }

    static constexpr field_semantics semantics() { return sem; }
    static constexpr bool is_unknown() { return true; }

    template <bool has_handler>
    [[gnu::always_inline]] static constexpr std::optional<uint32_t>
    classify_field_before_lexn(const ppb_field_meta &meta, bool *need_lexn)
    {
        // Truncating the sentinel tag is mostly fine: it's used for
        // `field_semantics::error`, which catch-alls may use, but
        // it's pretty rare to have 2**29 - 1 as a valid field number.
        return classify_field_before_lexn_impl<uint32_t(unknown::tag()), unknown::wire(),
            unknown::semantics(), has_handler>(meta, need_lexn);
    }

    // Cheap prescan-only report: re-decode the original tag varint
    // at `field.v.ptr` to populate `reader::unknown_field()` with the
    // first unknown tag seen.  The C lexer rewrites the per-field
    // state's tag to a wire-only sentinel, so this re-decode is the
    // only way to recover the field number.
    [[gnu::always_inline]] static constexpr void maybe_flag_unknown_field(const ppb_field &field,
        std::span<const std::byte> input, uint64_t *unknown_field)
    {
        if (field.m.num_occurrences == 0 || *unknown_field != 0)
            return;

        const auto *ptr = static_cast<const std::byte *>(field.v.ptr);
        const std::byte *end = input.data() + input.size();

        size_t available = (uintptr_t(ptr) - uintptr_t(input.data()) <= input.size()) ? size_t(end - ptr) : 0;
        ppb_buf buf = { .buf = ptr, .size = available };
        ppb_error err = PPB_OK;
        uint64_t tag = ppb_decode_varint(&buf, &err);
        if (err == PPB_OK)
            *unknown_field = tag;
    }
};

// Typed scalar wrappers: each decodes its wire-typed base into a
// native C++ value via `extract_value`.
template <auto K, field_semantics sem> struct int32 : public varint<K, sem>
{
    static constexpr int32_t extract_value(const ppb_field &f, ppb_error *)
    {
        return static_cast<int32_t>(f.v.u64);
    }
};

template <auto K, field_semantics sem> struct int64 : public varint<K, sem>
{
    static constexpr int64_t extract_value(const ppb_field &f, ppb_error *)
    {
        return static_cast<int64_t>(f.v.u64);
    }
};

template <auto K, field_semantics sem> struct sint32 : public varint<K, sem>
{
    static constexpr int32_t extract_value(const ppb_field &f, ppb_error *)
    {
        return ppb_zag32(static_cast<uint32_t>(f.v.u64));
    }
};

template <auto K, field_semantics sem> struct sint64 : public varint<K, sem>
{
    static constexpr int64_t extract_value(const ppb_field &f, ppb_error *) { return ppb_zag(f.v.u64); }
};

template <auto K, field_semantics sem> struct uint32 : public varint<K, sem>
{
    static constexpr uint32_t extract_value(const ppb_field &f, ppb_error *)
    {
        return static_cast<uint32_t>(f.v.u64);
    }
};

template <auto K, field_semantics sem> struct uint64 : public varint<K, sem>
{
    static constexpr uint64_t extract_value(const ppb_field &f, ppb_error *) { return f.v.u64; }
};

template <auto K, field_semantics sem> struct boolean : public varint<K, sem>
{
    static constexpr bool extract_value(const ppb_field &f, ppb_error *) { return f.v.u64 != 0; }
};

namespace detail
{

// True for an enum with a *fixed* underlying type (a scoped enum, or
// `enum E : T`).  Such enums can cast from any underlying-type value
// without UB, unlike plain unscoped enums. Required so that unchecked
// wire values are always safe to cast.
//
// `T{u}` list-init is well-formed iff the type is fixed (P0138R2); the
// `underlying_type_t<T>` probe never narrows, so it cannot spuriously
// reject a fixed enum.
template <typename T>
concept fixed_underlying_enum = enum_type<T> && requires(std::underlying_type_t<T> u) { T { u }; };

}  // namespace detail

template <auto K, typename Enum, field_semantics sem, typename UnderlyingType>
    requires detail::enum_type<Enum>
struct enumerated : public varint<K, sem>
{
    static_assert(std::is_enum_v<Enum>, "enumerated field type requires an enum type parameter");
    static_assert(detail::fixed_underlying_enum<Enum>,
        "enumerated field requires an enum with a fixed underlying type (a scoped enum, or "
        "`enum E : T`): casting an out-of-range wire value would otherwise be undefined behavior");

    // Narrow to `UnderlyingType`, then to `Enum`: well-defined for any
    // 64-bit input because `fixed_underlying_enum<Enum>` is enforced
    // above.  Out-of-range values reach the handler as that bit pattern.
    static constexpr Enum extract_value(const ppb_field &f, ppb_error *)
    {
        return static_cast<Enum>(static_cast<UnderlyingType>(f.v.u64));
    }
};

template <auto K, field_semantics sem> struct fixed32 : public i32<K, sem>
{
    static constexpr uint32_t extract_value(const ppb_field &f, ppb_error *)
    {
        return static_cast<uint32_t>(f.v.u64);
    }
};

template <auto K, field_semantics sem> struct sfixed32 : public i32<K, sem>
{
    static constexpr int32_t extract_value(const ppb_field &f, ppb_error *)
    {
        return std::bit_cast<int32_t>(static_cast<uint32_t>(f.v.u64));
    }
};

template <auto K, field_semantics sem> struct f32 : public i32<K, sem>
{
    static constexpr float extract_value(const ppb_field &f, ppb_error *)
    {
        return std::bit_cast<float>(static_cast<uint32_t>(f.v.u64));
    }
};

template <auto K, field_semantics sem> struct fixed64 : public i64<K, sem>
{
    static constexpr uint64_t extract_value(const ppb_field &f, ppb_error *) { return f.v.u64; }
};

template <auto K, field_semantics sem> struct sfixed64 : public i64<K, sem>
{
    static constexpr int64_t extract_value(const ppb_field &f, ppb_error *)
    {
        return std::bit_cast<int64_t>(f.v.u64);
    }
};

template <auto K, field_semantics sem> struct f64 : public i64<K, sem>
{
    static constexpr double extract_value(const ppb_field &f, ppb_error *)
    {
        return std::bit_cast<double>(f.v.u64);
    }
};

// LEN field exposed as a `std::string_view` over the payload bytes.
// The wrapper does *not* validate UTF-8; callers who care must check
// themselves.
template <auto K, field_semantics sem> struct utf8string : public len<K, sem>
{
    static constexpr std::string_view extract_value(const ppb_field &f, ppb_error *)
    {
        return std::string_view(static_cast<const char *>(f.v.payload.buf), f.v.payload.size);
    }
};

template <auto K, typename Element, field_semantics sem> struct bytes : public len<K, sem>
{
    static_assert(alignof(Element) == 1, "bytes element type must be byte-aligned");

    // Reinterpret the LEN payload as a span of `Element`.  Safe only
    // when `alignof(Element) == 1` (enforced above) and the payload
    // length is a multiple of `sizeof(Element)`; otherwise we set
    // `PPB_ERROR_TRUNCATED_DATA` and hand back an empty span.
    static constexpr std::span<const Element> extract_value(const ppb_field &f, ppb_error *error)
    {
        if (f.v.payload.size % sizeof(Element) != 0) [[unlikely]]
        {
            if (*error == PPB_OK)
                *error = PPB_ERROR_TRUNCATED_DATA;

            return {};
        }

        size_t count = f.v.payload.size / sizeof(Element);
        return std::span<const Element>(static_cast<const Element *>(f.v.payload.buf), count);
    }
};

// Sub-message field: a `bytes`-shaped field that additionally
// carries its payload's schema as `inner_schema`.  Identical to
// `bytes` on the wire; `ppb::on_submessage<K, S>` cross-checks `S`
// against `inner_schema` at compile time.  `on_submessage<K, S>`
// also binds to plain `bytes<K>` / `utf8string<K>` / `len<K>`
// fields, but without the static schema check.
template <auto K, typename InnerSchema, field_semantics sem> struct message : public bytes<K, std::byte, sem>
{
    static_assert(sem != field_semantics::proto3_zero_default,
        "submessages must not have proto3_zero_default semantics (last_write_wins matches both proto2 and 3)");

    using inner_schema = InnerSchema;
};

template <typename T>
[[gnu::always_inline]] inline T
le_packed<T>::value() const
{
    if constexpr (std::endian::native == std::endian::little)
    {
        T result;
        std::memcpy(&result, &data, sizeof(T));
        return result;
    }
    else if constexpr (std::endian::native == std::endian::big)
    {
        if constexpr (sizeof(T) == 4)
        {
            uint32_t raw;
            std::memcpy(&raw, &data, sizeof(T));
            raw = __builtin_bswap32(raw);
            return std::bit_cast<T>(raw);
        }
        else if constexpr (sizeof(T) == 8)
        {
            uint64_t raw;
            std::memcpy(&raw, &data, sizeof(T));
            raw = __builtin_bswap64(raw);
            return std::bit_cast<T>(raw);
        }
        else
        {
            static_assert(sizeof(T) == 4 || sizeof(T) == 8,
                "le_packed only has byte-swapping logic for 4 and 8 bytes");
        }
    }
    else
    {
        static_assert(sizeof(T) != sizeof(T), "ppb only supports little- and big-endian platforms");
        return T {};
    }
}

namespace detail
{

// True when `F` is a `ppb::message<...>` (sub-message field that
// carries its inner schema).
template <typename F> inline constexpr bool is_message_field_v = requires { typename F::inner_schema; };

// Picks the schema's `Key` as the first field's `Key` typedef.
// Catch-alls (`ppb::unknown<wire>`) have none and are skipped; a
// catch-all-only schema falls back to `int`, which is harmless since
// such schemas never dispatch on a typed Key.
template <typename...> struct key_of
{
    using type = int;
};

template <typename First, typename... Rest> struct key_of<First, Rest...>
{
private:
    static consteval auto pick()
    {
        if constexpr (requires { typename First::Key; })
            return std::type_identity<typename First::Key> {};
        else
            return std::type_identity<typename key_of<Rest...>::type> {};
    }

public:
    using type = typename decltype(pick())::type;
};

template <typename F, typename Key>
consteval bool
field_key_matches()
{
    // Catch-alls expose no `Key`; treat them as matching everything.
    if constexpr (requires { typename F::Key; })
        return std::is_same_v<typename F::Key, Key>;
    else
        return true;
}

template <typename... Fs> struct schema_impl
{
    using Key = typename key_of<Fs...>::type;

    static constexpr size_t num_fields() { return sizeof...(Fs); }

    static consteval bool fields_non_empty() { return num_fields() > 0; }

    static consteval bool fields_are_fields() { return (std::is_base_of_v<field_generic_base, Fs> && ...); }

    static consteval bool fields_have_same_key_type() { return (field_key_matches<Fs, Key>() && ...); }

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

    // Returns a field_range covering all schema entries that match
    // `k`.  When `wire` is not `wire_type::any`, only entries with
    // that concrete wire type are counted.
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

// Folds `upd` into `acc`.  Used by `reader::meta<key>()` to merge
// metadata across wire types when one field number is listed under
// several.  When both sides have occurrences we set
// `lost_distinct_u64`: we can't compare across wire types, so be
// conservative.
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
    // (x - 1) maps 0 to SIZE_MAX so a 0 never replaces a real value.
    acc.min_nonzero_bytes = (acc.min_nonzero_bytes - 1 > upd.min_nonzero_bytes - 1) ? upd.min_nonzero_bytes :
                                                                                      acc.min_nonzero_bytes;
    acc.max_bytes = (acc.max_bytes < upd.max_bytes) ? upd.max_bytes : acc.max_bytes;
}

// Resolves (key, wire) to a single schema slot index, or fails to
// compile.  `wire == any` is only acceptable when exactly one schema
// entry has that key; otherwise the caller must disambiguate.
template <typename Schema, typename Schema::Key key, wire_type wire>
consteval size_t
find_single_field_index()
{
    constexpr typename Schema::field_range range = Schema::find_field_range(key, wire);

    static_assert(range.count > 0, "Key/wire combination not found in schema");
    static_assert(range.count == 1,
        "Key matches multiple wire types in the schema; "
        "specify a wire type explicitly (e.g. field<K, ppb::wire_type::varint>())");
    static_assert(range.begin < Schema::num_fields(), "internal: index out of bounds");

    return range.begin;
}

// Compile-time-bounded dispatch over [begin, end) in [0, limit).
//
// The body is split into 16-wide blocks; within each block a switch
// on the local offset jumps into a fall-through chain that invokes
// `handler(integral_constant<size_t, I>{})` for each I from the entry
// point onward.  The handler returns `bool`: `true` to continue,
// `false` to stop early (the outer `dispatch` then skips later
// blocks).
//
// `begin` and `end` are runtime bounds, so a single instantiation
// covers every half-open subrange of [0, limit).
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
                // Unreachable: the preceding I (where
                // `base + I + 1 == limit`) returns false, stopping
                // `dispatch_16`'s fall-through chain before we get here.
                __builtin_unreachable();  // LCOV_EXCL_LINE
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

// Empty tag base shared by `value_handler`, `unknown_handler`, and
// `submessage_handler`.  `detail::is_handler_v` checks for it.
struct handler_base
{
};

// `value_handler<Key, W, Fn>` pairs a callable with the (Key, wire)
// pattern it matches.  Built by `ppb::on<Key, wire>(callable)`; the
// matching/dispatch logic lives in `find_value_handler` below.
template <auto K, wire_type W, typename Fn> struct value_handler : handler_base
{
    using key_type = decltype(K);
    using handler_type = Fn;

    static constexpr key_type key() { return K; }
    static constexpr wire_type wire() { return W; }
    static constexpr bool is_unknown_handler() { return false; }
    static constexpr bool is_submessage_handler() { return false; }

    // True when this handler's (Key, wire) matches the field's.
    static constexpr bool matches(key_type other_key, wire_type other_w)
    {
        if (K != other_key)
            return false;

        return (W == wire_type::any) || (W == other_w);
    }

    Fn handler;
};

// Sibling of `value_handler` for catch-all dispatch via
// `ppb::on_unknown<wire>(fn)`.  Carries no `key_type` so the
// same-Key-type check in `find_value_handler` skips it.  The handler
// receives the raw `const ppb_field &` from the C lexer; callers can
// recover the encoded tag at `field.v.ptr` via `ppb_decode_varint`.
template <wire_type W, typename Fn> struct unknown_handler : handler_base
{
    using handler_type = Fn;

    static constexpr wire_type wire() { return W; }
    static constexpr bool is_unknown_handler() { return true; }
    static constexpr bool is_submessage_handler() { return false; }

    // Wire-only match.  `wire_type::any` matches every catch-all
    // wire, mirroring `value_handler::matches`.
    static constexpr bool matches_wire(wire_type other_w) { return (W == wire_type::any) || (W == other_w); }

    Fn handler;
};

// True when T (after decay) is one of our handler types: a
// value_handler, unknown_handler, or submessage_handler.
template <typename T> inline constexpr bool is_handler_v = std::is_base_of_v<handler_base, std::decay_t<T>>;

// Synthetic `handler_type` for `submessage_handler`: invocable with
// any `Arg`, so `find_value_handler`'s `is_invocable_v` check never
// rejects a submessage handler regardless of which LEN-shaped field
// it's matched to (string_view, span<std::byte>, const ppb_field &).
// Never actually called: real dispatch goes through
// `submessage_handler::invoke(...)`.
struct submessage_invoker
{
    template <typename T> ppb_error operator()(T &&) const noexcept { return PPB_OK; }
};

// Product of `ppb::on_submessage<K, InnerSchema>`.  Inherits from
// `value_handler<K, wire_type::len, submessage_invoker>` to reuse
// `find_value_handler`'s key/wire matching; the
// `is_submessage_handler()` override routes dispatch to `invoke()`
// in `run_handler_for_idx` instead of the usual `handler(arg)` path.
//
// `Init = std::nullopt_t` when no init was supplied; inner
// `parse(std::nullopt, ...)` then skips the inner-init call.
template <auto K, typename InnerSchema, typename Init, typename... Hs>
struct submessage_handler : public value_handler<K, wire_type::len, submessage_invoker>
{
    static constexpr bool is_submessage_handler() { return true; }
    using inner_schema_type = InnerSchema;

    Init init_cb;
    std::tuple<Hs...> inner_handlers;

    template <typename InitArg>
    constexpr submessage_handler(InitArg &&i, std::tuple<Hs...> hs)
        : value_handler<K, wire_type::len, submessage_invoker> { {}, submessage_invoker {} }
        , init_cb(std::forward<InitArg>(i))
        , inner_handlers(std::move(hs))
    {
    }

    [[gnu::always_inline]] inline ppb_error invoke(std::span<const std::byte> payload,
        const limit &active_bounds, const std::byte *outer_end);
};

// Does `H`'s key type match `DispatchKey`?  Always true for
// `unknown_handler`, which has no `key_type`.
template <typename H, typename DispatchKey>
consteval bool
handler_key_type_ok()
{
    if constexpr (H::is_unknown_handler())
        return true;
    else
        return std::is_same_v<std::decay_t<typename H::key_type>, std::decay_t<DispatchKey>>;
}

// Per-handler `(Key, wire)` match.  Returns false for
// `unknown_handler`, which never participates in typed dispatch.
template <typename H, auto Key, wire_type W>
consteval bool
handler_matches_key_wire()
{
    if constexpr (H::is_unknown_handler())
        return false;
    else
        return H::matches(Key, W);
}

// Picks the handler in `Hs...` that matches the `(Key, W)` pair and
// is invocable with `Arg`.
//   - `nullopt` when no handler matches `(Key, W)`: silently dropping
//     unhandled fields is fine.
//   - exactly one match -> return its index.
//   - multiple `(Key, W)` matches -> disambiguate by
//     `is_invocable_v<handler_type, Arg>` and require a single
//     survivor; zero or many survivors are `static_assert` errors.
template <auto Key, wire_type W, typename Arg, typename... Hs>
consteval std::optional<size_t>
find_value_handler()
{
    static_assert((handler_key_type_ok<Hs, decltype(Key)>() && ...),
        "every value_handler in the tuple must use the same key type as the dispatch Key");

    constexpr size_t N = sizeof...(Hs);

    struct hit
    {
        size_t count;
        size_t first;
    };

    constexpr auto scan = [](const std::array<bool, N> &m1, const std::array<bool, N> &m2) -> hit
    {
        hit r = { 0, N };
        for (size_t i = 0; i < N; i++)
        {
            if (!(m1[i] && m2[i]))
                continue;

            if (r.count == 0)
                r.first = i;

            r.count++;
        }

        return r;
    };

    constexpr std::array<bool, N> key_mask = { handler_matches_key_wire<Hs, Key, W>()... };
    constexpr hit key_hit = scan(key_mask, key_mask);

    if constexpr (key_hit.count == 0)
    {
        return std::nullopt;
    }
    else if constexpr (key_hit.count == 1)
    {
        return key_hit.first;
    }
    else
    {
        constexpr std::array<bool, N> arg_mask = { std::is_invocable_v<typename Hs::handler_type, Arg>... };
        constexpr hit arg_hit = scan(key_mask, arg_mask);

        static_assert(arg_hit.count != 0,
            "value_handlers match (Key, wire), but none is invocable with the argument type");
        static_assert(arg_hit.count <= 1,
            "multiple value_handlers match (Key, wire) and accept the argument type; ambiguous");

        return arg_hit.first;
    }
}

// Tuple-deducing shorthand: the parameter just deduces `Hs...` from a
// tuple the caller already has.  Only valid when the tuple is itself
// a constant expression (e.g. in tests); inside a function with a
// runtime tuple, call the type-list form directly.
template <auto Key, wire_type W, typename Arg, typename... Hs>
consteval std::optional<size_t>
find_value_handler(const std::tuple<Hs...> &)
{
    return find_value_handler<Key, W, Arg, Hs...>();
}

// Per-handler wire-only match for catch-all dispatch.  Returns false
// for everything that isn't an `unknown_handler`.
template <typename H, wire_type W>
consteval bool
unknown_handler_matches_wire()
{
    if constexpr (H::is_unknown_handler())
        return H::matches_wire(W);
    else
        return false;
}

// `find_value_handler`'s sibling for catch-alls: picks the
// `unknown_handler` in `Hs...` whose `wire()` matches `W` and is
// invocable with `Arg`.  Same match/disambiguation rules, minus the
// key check.
template <wire_type W, typename Arg, typename... Hs>
consteval std::optional<size_t>
find_unknown_handler()
{
    constexpr size_t N = sizeof...(Hs);

    struct hit
    {
        size_t count;
        size_t first;
    };

    constexpr auto scan = [](const std::array<bool, N> &m1, const std::array<bool, N> &m2) -> hit
    {
        hit r = { 0, N };
        for (size_t i = 0; i < N; i++)
        {
            if (!(m1[i] && m2[i]))
                continue;

            if (r.count == 0)
                r.first = i;

            r.count++;
        }

        return r;
    };

    constexpr std::array<bool, N> wire_mask = { unknown_handler_matches_wire<Hs, W>()... };
    constexpr hit wire_hit = scan(wire_mask, wire_mask);

    if constexpr (wire_hit.count == 0)
    {
        return std::nullopt;
    }
    else if constexpr (wire_hit.count == 1)
    {
        return wire_hit.first;
    }
    else
    {
        constexpr std::array<bool, N> arg_mask = { std::is_invocable_v<typename Hs::handler_type, Arg>... };
        constexpr hit arg_hit = scan(wire_mask, arg_mask);

        static_assert(arg_hit.count != 0,
            "unknown_handlers match the wire type, but none is invocable with the argument type");
        static_assert(arg_hit.count <= 1,
            "multiple unknown_handlers match the wire type and accept the argument type; ambiguous");

        return arg_hit.first;
    }
}

// True when handler `H` matches at least one field in `Fs...`.
// `reader::run_handlers` uses this to flag orphaned
// `on()`/`on_unknown()` handlers as compile errors.
template <typename H, typename... Fs>
consteval bool
handler_matches_some_field()
{
    constexpr auto check_one = []<typename F>() consteval -> bool
    {
        if constexpr (H::is_unknown_handler())
        {
            if constexpr (F::is_unknown())
                return H::matches_wire(F::wire());

            return false;
        }
        else
        {
            if constexpr (F::is_unknown())
            {
                return false;
            }
            else if constexpr (!std::is_same_v<std::decay_t<typename H::key_type>,
                                   std::decay_t<typename F::Key>>)
            {
                // Key-type mismatch would make `H::matches()` fail to
                // compile.  Return true so the dedicated key-type
                // assert in `find_value_handler` fires with a clearer
                // diagnostic than "handler matches no schema field".
                return true;
            }
            else
            {
                return H::matches(F::tag(), F::wire());
            }
        }
    };

    return (... || check_one.template operator()<Fs>());
}

// Decode policies for packed varint iterators.
struct identity_decode
{
    template <typename T> static constexpr T apply(uint64_t v) noexcept { return static_cast<T>(v); }
};

struct zigzag_decode
{
    template <typename T> static constexpr T apply(uint64_t v) noexcept
    {
        /*
         * Truncate the zigzag-encoded value to unsigned T before
         * decoding: this is needed for sint32 (to handle non-canonical
         * encodings the same way as google's C++ parser), and a no-op
         * for sint64.
         */
        return static_cast<T>(ppb_zag(static_cast<std::make_unsigned_t<T>>(v)));
    }
};

template <typename UnderlyingType> struct enum_decode
{
    template <typename T> static constexpr T apply(uint64_t v) noexcept
    {
        return static_cast<T>(static_cast<UnderlyingType>(v));
    }
};

// Lazily decoded view over a packed-varint LEN payload, handed to
// handlers for `packed_int32`/`packed_uint64`/etc.  Captures
// `&reader::m_error`: on decode failure
// (`PPB_ERROR_TRUNCATED_DATA`/`_CORRUPT_VARINT`) the iterator folds
// the error into the reader (first-write-wins) and exhausts.  The
// captured byte range lives in the input span; the error pointer
// lives in the reader.
//
// Must not outlive the callback that received the parent
// `packed_varint_view` from `reader::dispatch()`.
template <typename T, typename Policy> class packed_varint_iter
{
public:
    using value_type = T;
    using reference = T;
    using pointer = const T *;
    using iterator_category = std::input_iterator_tag;
    using difference_type = ptrdiff_t;

    packed_varint_iter() = default;

    packed_varint_iter(const std::byte *pos, const std::byte *end, ppb_error *error) noexcept
        : m_pos(pos)
        , m_next(pos)
        , m_end(end)
        , m_error(error)
    {
        if (m_pos < m_end) [[likely]]
            decode();
    }

    T operator*() const noexcept { return m_current; }

    packed_varint_iter &operator++()
    {
        m_pos = m_next;
        if (m_pos < m_end) [[likely]]
            decode();

        return *this;
    }

    packed_varint_iter operator++(int)
    {
        packed_varint_iter tmp = *this;
        ++(*this);
        return tmp;
    }

    packed_varint_iter end() const { return packed_varint_iter(m_end, m_end, m_error); }

    bool operator==(const packed_varint_iter &other) const noexcept { return m_pos == other.m_pos; }
    bool operator!=(const packed_varint_iter &other) const noexcept { return m_pos != other.m_pos; }

private:
    // Decode the varint at `m_pos` into `m_current` and point `m_next`
    // at its end.  `m_pos` is left untouched so end-iterator equality
    // only triggers after `operator++` consumes the current value.
    void decode() noexcept
    {
        size_t remaining = size_t(m_end - m_pos);
        ppb_buf buf = { .buf = m_pos, .size = remaining };
        ppb_error err = PPB_OK;
        uint64_t raw = ppb_decode_varint(&buf, &err);

        if (err != PPB_OK) [[unlikely]]
        {
            if (*m_error == PPB_OK)
                *m_error = err;

            m_pos = m_end;
            m_next = m_end;
            return;
        }

        m_current = Policy::template apply<T>(raw);
        m_next = static_cast<const std::byte *>(buf.buf);
    }

    const std::byte *m_pos = nullptr;
    const std::byte *m_next = nullptr;
    const std::byte *m_end = nullptr;
    ppb_error *m_error = nullptr;
    T m_current = T {};
};

// Lazily decoded view over a packed-varint LEN payload, handed to
// handlers for `packed_int32`/`packed_uint64`/etc. The captured byte
// range lives in the input span; the error pointer lives in the
// reader.
//
// Must not outlive the callback that received the parent
// `packed_varint_view` from `reader::dispatch()`.
template <typename T, typename Policy> class packed_varint_view
{
public:
    using iterator = packed_varint_iter<T, Policy>;

    packed_varint_view(const std::byte *data, size_t size, ppb_error *error) noexcept
        : m_begin(data, data + size, error)
    {
    }

    iterator begin() const noexcept { return m_begin; }
    iterator end() const noexcept { return m_begin.end(); }

private:
    iterator m_begin;
};

// True for span<const le_packed<T>> and packed_varint_view<T, Policy>.
// Used by ppb::push_back / ppb::emplace_back to iterate element-by-element
// instead of pushing the view as a single value.
template <typename T> inline constexpr bool is_packed_range_v = false;

template <typename T> inline constexpr bool is_packed_range_v<std::span<const le_packed<T>>> = true;

template <typename T, typename Policy>
inline constexpr bool is_packed_range_v<packed_varint_view<T, Policy>> = true;

/* Invoke a handler that may return ppb_error or void; void folds to PPB_OK. */
template <typename Fn, typename Arg>
[[nodiscard]] constexpr ppb_error
fold_call(Fn &fn, Arg &&arg)
{
    using result_type = decltype(fn(std::forward<Arg>(arg)));
    static_assert(std::is_void_v<result_type> || std::is_same_v<std::decay_t<result_type>, ppb_error>,
        "ppb handler callables must return void or ppb_error");

    if constexpr (std::is_void_v<result_type>)
    {
        fn(std::forward<Arg>(arg));
        return PPB_OK;
    }
    else
    {
        return fn(std::forward<Arg>(arg));
    }
}

template <typename> inline constexpr bool is_le_packed_v = false;
template <typename T> inline constexpr bool is_le_packed_v<ppb::le_packed<T>> = true;

/*
 * le_packed<T> -> T; everything else passes through.  Lets on_each hand a
 * single element type to the callback for both wire forms.
 */
template <typename E>
[[nodiscard]] constexpr auto
normalize_element(E &&e)
{
    if constexpr (is_le_packed_v<std::decay_t<E>>)
    {
        return e.value();
    }
    else
    {
        return std::forward<E>(e);
    }
}

}  // namespace detail

// Packed repeated field types.  Fixed-width variants decode into a
// span of `le_packed<T>`; varint variants below use the lazy
// `packed_varint_view`.

template <auto K, field_semantics sem> struct packed_fixed32 : public bytes<K, le_packed<uint32_t>, sem>
{
};

template <auto K, field_semantics sem> struct packed_sfixed32 : public bytes<K, le_packed<int32_t>, sem>
{
};

template <auto K, field_semantics sem> struct packed_f32 : public bytes<K, le_packed<float>, sem>
{
};

template <auto K, field_semantics sem> struct packed_fixed64 : public bytes<K, le_packed<uint64_t>, sem>
{
};

template <auto K, field_semantics sem> struct packed_sfixed64 : public bytes<K, le_packed<int64_t>, sem>
{
};

template <auto K, field_semantics sem> struct packed_f64 : public bytes<K, le_packed<double>, sem>
{
};

// Packed varint fields return a lazy-decoding view.

template <auto K, field_semantics sem> struct packed_int32 : public len<K, sem>
{
    static constexpr auto extract_value(const ppb_field &f, ppb_error *error)
    {
        return detail::packed_varint_view<int32_t, detail::identity_decode>(static_cast<const std::byte *>(
                                                                                f.v.payload.buf),
            f.v.payload.size, error);
    }
};

template <auto K, field_semantics sem> struct packed_int64 : public len<K, sem>
{
    static constexpr auto extract_value(const ppb_field &f, ppb_error *error)
    {
        return detail::packed_varint_view<int64_t, detail::identity_decode>(static_cast<const std::byte *>(
                                                                                f.v.payload.buf),
            f.v.payload.size, error);
    }
};

template <auto K, field_semantics sem> struct packed_sint32 : public len<K, sem>
{
    static constexpr auto extract_value(const ppb_field &f, ppb_error *error)
    {
        return detail::packed_varint_view<int32_t, detail::zigzag_decode>(static_cast<const std::byte *>(
                                                                              f.v.payload.buf),
            f.v.payload.size, error);
    }
};

template <auto K, field_semantics sem> struct packed_sint64 : public len<K, sem>
{
    static constexpr auto extract_value(const ppb_field &f, ppb_error *error)
    {
        return detail::packed_varint_view<int64_t, detail::zigzag_decode>(static_cast<const std::byte *>(
                                                                              f.v.payload.buf),
            f.v.payload.size, error);
    }
};

template <auto K, field_semantics sem> struct packed_uint32 : public len<K, sem>
{
    static constexpr auto extract_value(const ppb_field &f, ppb_error *error)
    {
        return detail::packed_varint_view<uint32_t, detail::identity_decode>(static_cast<const std::byte *>(
                                                                                 f.v.payload.buf),
            f.v.payload.size, error);
    }
};

template <auto K, field_semantics sem> struct packed_uint64 : public len<K, sem>
{
    static constexpr auto extract_value(const ppb_field &f, ppb_error *error)
    {
        return detail::packed_varint_view<uint64_t, detail::identity_decode>(static_cast<const std::byte *>(
                                                                                 f.v.payload.buf),
            f.v.payload.size, error);
    }
};

template <auto K, field_semantics sem> struct packed_boolean : public len<K, sem>
{
    static constexpr auto extract_value(const ppb_field &f, ppb_error *error)
    {
        return detail::packed_varint_view<bool, detail::identity_decode>(static_cast<const std::byte *>(
                                                                             f.v.payload.buf),
            f.v.payload.size, error);
    }
};

template <auto K, typename Enum, field_semantics sem, typename UnderlyingType>
    requires detail::enum_type<Enum>
struct packed_enumerated : public len<K, sem>
{
    static_assert(std::is_enum_v<Enum>, "enumerated field type requires an enum type parameter");
    static_assert(detail::fixed_underlying_enum<Enum>,
        "enumerated field requires an enum with a fixed underlying type (a scoped enum, or "
        "`enum E : T`): casting an out-of-range wire value would otherwise be undefined behavior");

    static constexpr auto extract_value(const ppb_field &f, ppb_error *error)
    {
        return detail::packed_varint_view<Enum, detail::enum_decode<UnderlyingType>>(
            static_cast<const std::byte *>(f.v.payload.buf), f.v.payload.size, error);
    }
};

namespace detail
{

// `flatten_one<T>::type` is `std::tuple<T>` for a leaf and the
// recursive flattening of any `std::tuple<...>`.  `flatten_t<Ts...>`
// then yields one `std::tuple<...>` of the leaves in left-to-right
// order.
template <typename T> struct flatten_one
{
    using type = std::tuple<T>;
};

template <typename... Ts> struct flatten_one<std::tuple<Ts...>>
{
    using type = decltype(std::tuple_cat(typename flatten_one<Ts>::type {}...));
};

// A complete schema flattens to its field pack, so auto_schema can
// splice and re-sort it (e.g. extending a generated WKT schema with
// detect_unknown_fields<> at the use site).
template <typename... Fs> struct flatten_one<schema<Fs...>>
{
    using type = std::tuple<Fs...>;
};

template <typename... Ts> using flatten_t = decltype(std::tuple_cat(typename flatten_one<Ts>::type {}...));

template <typename Tuple> struct sort_fields;

// Validates every leaf as a `field_generic_base` subclass, then
// computes the permutation that sorts them by
// `(uint64_t(key), wire, original_index)` -- the strictly-ascending
// encoded-tag order that `schema<>` will then re-check.
//
// `to<Tmpl>` rebinds the sorted pack onto an arbitrary
// `template <typename...> class`, letting `auto_schema` splice the
// sorted fields into `schema<...>` without exposing an index-sequence
// parameter on its own signature.
template <typename... Fs> struct sort_fields<std::tuple<Fs...>>
{
    static_assert((std::is_base_of_v<field_generic_base, Fs> && ...),
        "auto_schema arguments must be field_generic_base (possibly nested in std::tuple)");

    static consteval std::array<size_t, sizeof...(Fs)> permutation()
    {
        constexpr size_t n = sizeof...(Fs);
        using entry = std::tuple<uint64_t, uint8_t, size_t>;

        return [&]<size_t... Is>(std::index_sequence<Is...>)
        {
            std::array<entry, n> entries = { entry { static_cast<uint64_t>(Fs::tag()),
                static_cast<uint8_t>(Fs::wire()), Is }... };

            std::sort(entries.begin(), entries.end());

            std::array<size_t, n> perm = {};
            for (size_t i = 0; i < n; i++)
                perm[i] = std::get<2>(entries[i]);
            return perm;
        }(std::make_index_sequence<n> {});
    }

    template <template <typename...> class Tmpl, typename Idx> struct rebind;

    template <template <typename...> class Tmpl, size_t... Is> struct rebind<Tmpl, std::index_sequence<Is...>>
    {
        using type = Tmpl<std::tuple_element_t<permutation()[Is], std::tuple<Fs...>>...>;
    };

    template <template <typename...> class Tmpl>
    using to = typename rebind<Tmpl, std::make_index_sequence<sizeof...(Fs)>>::type;
};

// Convenience interface: flattens the input pack and forwards to `sort_fields`.
// `sorted_fields<Ts...>::template to<Tmpl>` yields `Tmpl<sorted Fs...>`.
template <typename... Ts> using sorted_fields = sort_fields<flatten_t<Ts...>>;

}  // namespace detail
}  // namespace ppb
