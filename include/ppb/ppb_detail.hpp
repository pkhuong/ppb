// Private implementation details for the PPB C++ wrapper
//
// This header is included exactly once by <ppb/ppb.hpp>, at the point
// where the public forward declarations are visible but the
// `ppb::schema`/`ppb::reader` definitions are not yet introduced.  Do
// not include it directly; nothing in here is a stable API.
//
// Identifiers in `ppb::detail::` are not part of the API surface and
// may change without notice.
namespace ppb
{

// Base for every field descriptor.  Most fields inherit via
// `field_base<K, wire_type>` (typed scalars, the four raw wire-typed
// primitives), which adds the `(K, wire_type)` static_asserts and
// exposes `tag()`, `wire()`, and `encoded_tag()`.  Catch-alls
// (`ppb::unknown<wire>`) inherit from `field_generic_base` directly
// so they can bypass the `[1, 2**29 - 1]` key-range constraint and
// skip the schema-wide same-Key-type check.
//
// All static methods are shadowed statically as needed.
struct field_generic_base
{
    static constexpr const ppb_field &extract_value(const ppb_field &field, ppb_error *) { return field; }
    static constexpr field_semantics semantics() { return field_semantics::always_lexn; }

    // Catch-all marker: shadowed by `ppb::unknown<>` to return true.
    // The reader uses this to identify catch-all entries during the
    // post-prescan field walk and the per-field handler dispatch,
    // without needing a separate trait.
    static constexpr bool is_unknown() { return false; }
};

// Common base for fields with a real (in-range) field number.
// Enforces the protobuf field-number range and a valid wire type.
//
// Field-tag constraints (enforced by static_assert):
//   - K convertible to uint64_t
//   - 1 <= K <= 2^29 - 1 (the protobuf-spec field-number range)
//
// Catch-all entries (`ppb::unknown<wire>`) sidestep this base and
// derive from `field_generic_base` directly.
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
};

template <auto K, field_semantics sem> struct i64 : public field_base<K, wire_type::i64>
{
    static constexpr field_semantics semantics() { return sem; }
};

template <auto K, field_semantics sem> struct len : public field_base<K, wire_type::len>
{
    static constexpr field_semantics semantics() { return sem; }
};

template <auto K, field_semantics sem> struct i32 : public field_base<K, wire_type::i32>
{
    static constexpr field_semantics semantics() { return sem; }
};

// Catch-all descriptor for the C lexer's `PPB_TAG(-1, wire)` entry:
// matches any tag with the given wire type that doesn't already have
// a dedicated descriptor in the schema.  Derives directly from
// `field_generic_base` so it bypasses `field_base`'s `[1, 2**29 - 1]`
// key-range check, and intentionally exposes no `Key` typedef so the
// schema-wide same-Key-type assertion ignores it.
//
// The default `field_semantics::repeated` makes it `on_unknown` handlers,
// when registered, see every single matching unknown field.  When there
// is no such handler, we won't lexn solely for unknown fields.
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
};

// Nicer scalar types (wrappers around the 4 wire types above)
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
        return static_cast<int32_t>(ppb_zag(f.v.u64));
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

template <auto K, typename Enum, field_semantics sem, typename UnderlyingType>
struct enumerated : public varint<K, sem>
{
    static_assert(std::is_enum_v<Enum>, "enumerated field type requires an enum type parameter");

    // Cast through the underlying type before reaching `Enum`.  This
    // is well-defined for any 64-bit input regardless of `Enum`'s
    // value range; out-of-range values reach the handler as the
    // corresponding bit pattern in `Enum`'s underlying type.
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

    // Reinterpret the LEN payload as a span of `Element`; this is
    // only safe for unaligned `Element`, and only works when the
    // payload buffer size is a multiple of the element size.
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

template <typename T>
[[gnu::always_inline]] inline T
le_packed<T>::value() const
{
    if constexpr (std::endian::native == std::endian::little)
    {
        return x;
    }

    if constexpr (sizeof(T) == 4)
    {
        uint32_t raw;
        std::memcpy(&raw, &x, sizeof(T));
        raw = __builtin_bswap32(raw);
        return std::bit_cast<T>(raw);
    }
    else
    {
        uint64_t raw;
        std::memcpy(&raw, &x, sizeof(T));
        raw = __builtin_bswap64(raw);
        return std::bit_cast<T>(raw);
    }
}

namespace detail
{

// Resolves the schema's `Key` type by walking the field pack and
// returning the first field's `Key` typedef.  Catch-all entries
// (`ppb::unknown<wire>`) have no `Key`, so they're skipped: a schema
// with catch-alls only resolves to the empty-pack fallback (`int`),
// which is harmless because catch-all-only schemas never need to
// match a typed dispatch Key.
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
    // Catch-all entries (e.g. `ppb::unknown<wire>`) intentionally
    // expose no `Key` typedef so we can treat them as matching any
    // Key type.
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

// Folds `upd` into `acc`.  Used by `reader::meta<key>()` to combine
// per-wire-type metadata when the schema has multiple entries sharing
// a field number.  Always sets `lost_distinct_u64` when both sides
// have at least one occurrence: we can't tell, so better to be
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
    acc.min_nonzero_bytes = (acc.min_nonzero_bytes - 1 > upd.min_nonzero_bytes - 1) ? upd.min_nonzero_bytes :
                                                                                      acc.min_nonzero_bytes;
    acc.max_bytes = (acc.max_bytes < upd.max_bytes) ? upd.max_bytes : acc.max_bytes;
}

// Compile-time-bounded dispatch over [begin, end) in [0, limit).
//
// The body is split into 16-wide blocks; within each block a switch
// on the local offset jumps into a fall-through chain that invokes
// `handler(integral_constant<size_t, I>{})` for each I in the block
// from the entry point onward.  The handler returns `bool`:
//   - `true`  to continue with the next index;
//   - `false` to stop early.  The outer `dispatch` then returns
//             `false` and skips later blocks.
//
// `beging` and `end` are runtime bounds, so the same instantiation
// handles arbitrary half-open subranges of [0, limit).
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
                // we have a guard at the top: if the block isn't
                // runnable, we skip early.  After that, the previous
                // entry would exit early before getting here.
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

// `value_handler<Key, W, Fn>` pairs a callable with the (Key, wire)
// pattern it matches.  Built by `ppb::on<Key, wire>(callable)`.
//
// `find_value_handler<Key, W, Arg, Hs...>()` selects the handler in
// `Hs...` that matches `(Key, W)` and is invocable with `Arg`:
//   - returns `nullopt` when no handler matches (Key, W) -- silently
//     dropping unhandled fields is fine;
//   - returns the index when exactly one handler matches;
//   - when multiple handlers match (Key, W), disambiguates by
//     `is_invocable_v<handler, Arg>` and requires exactly one
//     survivor.  Zero or many survivors are static_assert errors.
//
// All handlers in the pack must share the dispatch `Key`'s C++ type;
// mixing (e.g.) a plain `int` literal with an `enum class`-keyed
// schema is a static_assert error.
template <auto K, wire_type W, typename Fn> struct value_handler
{
    using key_type = decltype(K);
    using handler_type = Fn;

    static constexpr key_type key() { return K; }
    static constexpr wire_type wire() { return W; }
    static constexpr bool is_unknown_handler() { return false; }

    // Does this value handler match the key.
    static constexpr bool matches(key_type other_key, wire_type other_w)
    {
        if (K != other_key)
            return false;

        return (W == wire_type::any) || (W == other_w);
    }

    Fn handler;
};

// Sibling of `value_handler` for catch-all dispatch via
// `ppb::on_unknown<wire>(fn)`.  Carries no `key_type` (catch-alls
// have no schema-side Key), so `find_value_handler`'s "all handlers
// share the dispatch Key's type" check skips it.
//
// The handler is invoked with `const ppb_field &` (the raw match
// from the C lexer); `field.v.ptr` points at the original tag byte
// in the input buffer so callers who care can recover the encoded
// tag via `ppb_decode_varint`, just like `unknown_field()`.
template <wire_type W, typename Fn> struct unknown_handler
{
    using handler_type = Fn;

    static constexpr wire_type wire() { return W; }
    static constexpr bool is_unknown_handler() { return true; }

    // Wire-only match for catch-all dispatch.  `wire_type::any` here
    // means "every catch-all wire", mirroring the value_handler
    // semantics for typed handlers.
    static constexpr bool matches_wire(wire_type other_w) { return (W == wire_type::any) || (W == other_w); }

    Fn handler;
};

// Selects the value_handler in `Hs...` that matches the (Key, W) pair
// and is invocable with `Arg`.
//
// Returns std::nullopt when no handler matches the (Key, W) pair --
// silently dropping unhandled fields is fine.
//
// When exactly one handler matches by (Key, W), returns its index.
//
// When multiple handlers match by (Key, W), disambiguates by
// std::is_invocable_v<handler_type, Arg> and requires that exactly
// one survivor remain; static_asserts otherwise.
template <typename H, typename DispatchKey>
consteval bool
handler_key_type_ok()
{
    if constexpr (H::is_unknown_handler())
        return true;
    else
        return std::is_same_v<std::decay_t<typename H::key_type>, std::decay_t<DispatchKey>>;
}

// Per-handler `(Key, wire)` match for `find_value_handler`'s key_mask,
// tolerant of `unknown_handler`s (which never match a typed dispatch
// key).
template <typename H, auto Key, wire_type W>
consteval bool
handler_matches_key_wire()
{
    if constexpr (H::is_unknown_handler())
        return false;
    else
        return H::matches(Key, W);
}

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

// Tuple-deducing shorthand: the parameter is only used to deduce
// `Hs...` from a tuple expression that the caller already has.  Only
// safe to call from a context where the tuple is itself a constant
// expression (e.g., test files); inside a function whose tuple is a
// runtime value, call the type-list form directly.
template <auto Key, wire_type W, typename Arg, typename... Hs>
consteval std::optional<size_t>
find_value_handler(const std::tuple<Hs...> &)
{
    return find_value_handler<Key, W, Arg, Hs...>();
}

// Per-handler wire-only match for `find_unknown_handler`'s wire_mask.
// Mirrors `handler_matches_key_wire` but only fires for
// `unknown_handler`s (which is what catch-all dispatch consumes).
template <typename H, wire_type W>
consteval bool
unknown_handler_matches_wire()
{
    if constexpr (H::is_unknown_handler())
        return H::matches_wire(W);
    else
        return false;
}

// Selects the `unknown_handler` in `Hs...` whose `wire()` matches `W`
// (or is `wire_type::any`) and is invocable with `Arg`.  Same
// structure as `find_value_handler`, except no match on key, only on
// wire type.
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

// Checks that handler H matches at least one field in Fs...
// Used by reader::run_handlers to catch useless on()/on_unknown()
// handlers at compile time.
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
                /*
                 * The handler's Key type differs from this field's Key
                 * type, so H::matches() wouldn't compile.  Return true
                 * here: we want the dedicated key-type static_assert in
                 * find_value_handler to fire with a clearer diagnostic
                 * than "handler does not match any schema field".
                 */
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
    template <typename T> static constexpr T apply(uint64_t v) noexcept { return static_cast<T>(ppb_zag(v)); }
};

template <typename UnderlyingType> struct enum_decode
{
    template <typename T> static constexpr T apply(uint64_t v) noexcept
    {
        return static_cast<T>(static_cast<UnderlyingType>(v));
    }
};

// Lazily decoded view over a packed-varint LEN payload.
//
// Used for handlers that take `auto view` for `packed_int32`,
// `packed_uint64`, etc.  The view captures `&reader::m_error`; on a
// decode failure (`PPB_ERROR_TRUNCATED_DATA`/`_CORRUPT_VARINT`) the
// iterator sets the reader's error first-write-wins and exhausts.
//
// The captured byte range lives in the original input span and the
// error pointer points into the reader.
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
    /*
     * Decode the varint at `m_pos` into `m_current` and set `m_next`
     * to its end.  `m_pos` is left untouched so equality with the end
     * iterator only triggers after `operator++` consumes the value.
     */
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

}  // namespace detail

// Packed repeated field types: some need the packed_varint_view above.
// (these first few can just give a span back, so that's nice)

template <auto K> struct packed_fixed32 : public bytes<K, le_packed<uint32_t>, field_semantics::repeated>
{
};

template <auto K> struct packed_sfixed32 : public bytes<K, le_packed<int32_t>, field_semantics::repeated>
{
};

template <auto K> struct packed_f32 : public bytes<K, le_packed<float>, field_semantics::repeated>
{
};

template <auto K> struct packed_fixed64 : public bytes<K, le_packed<uint64_t>, field_semantics::repeated>
{
};

template <auto K> struct packed_sfixed64 : public bytes<K, le_packed<int64_t>, field_semantics::repeated>
{
};

template <auto K> struct packed_f64 : public bytes<K, le_packed<double>, field_semantics::repeated>
{
};

// Packed varint fields return a lazy-decoding view.

template <auto K> struct packed_int32 : public len<K, field_semantics::repeated>
{
    static constexpr auto extract_value(const ppb_field &f, ppb_error *error)
    {
        return detail::packed_varint_view<int32_t, detail::identity_decode>(static_cast<const std::byte *>(
                                                                                f.v.payload.buf),
            f.v.payload.size, error);
    }
};

template <auto K> struct packed_int64 : public len<K, field_semantics::repeated>
{
    static constexpr auto extract_value(const ppb_field &f, ppb_error *error)
    {
        return detail::packed_varint_view<int64_t, detail::identity_decode>(static_cast<const std::byte *>(
                                                                                f.v.payload.buf),
            f.v.payload.size, error);
    }
};

template <auto K> struct packed_sint32 : public len<K, field_semantics::repeated>
{
    static constexpr auto extract_value(const ppb_field &f, ppb_error *error)
    {
        return detail::packed_varint_view<int32_t, detail::zigzag_decode>(static_cast<const std::byte *>(
                                                                              f.v.payload.buf),
            f.v.payload.size, error);
    }
};

template <auto K> struct packed_sint64 : public len<K, field_semantics::repeated>
{
    static constexpr auto extract_value(const ppb_field &f, ppb_error *error)
    {
        return detail::packed_varint_view<int64_t, detail::zigzag_decode>(static_cast<const std::byte *>(
                                                                              f.v.payload.buf),
            f.v.payload.size, error);
    }
};

template <auto K> struct packed_uint32 : public len<K, field_semantics::repeated>
{
    static constexpr auto extract_value(const ppb_field &f, ppb_error *error)
    {
        return detail::packed_varint_view<uint32_t, detail::identity_decode>(static_cast<const std::byte *>(
                                                                                 f.v.payload.buf),
            f.v.payload.size, error);
    }
};

template <auto K> struct packed_uint64 : public len<K, field_semantics::repeated>
{
    static constexpr auto extract_value(const ppb_field &f, ppb_error *error)
    {
        return detail::packed_varint_view<uint64_t, detail::identity_decode>(static_cast<const std::byte *>(
                                                                                 f.v.payload.buf),
            f.v.payload.size, error);
    }
};

template <auto K> struct packed_boolean : public len<K, field_semantics::repeated>
{
    static constexpr auto extract_value(const ppb_field &f, ppb_error *error)
    {
        return detail::packed_varint_view<bool, detail::identity_decode>(static_cast<const std::byte *>(
                                                                             f.v.payload.buf),
            f.v.payload.size, error);
    }
};

template <auto K, typename Enum, typename UnderlyingType>
struct packed_enumerated : public len<K, field_semantics::repeated>
{
    static constexpr auto extract_value(const ppb_field &f, ppb_error *error)
    {
        return detail::packed_varint_view<Enum, detail::enum_decode<UnderlyingType>>(
            static_cast<const std::byte *>(f.v.payload.buf), f.v.payload.size, error);
    }
};

namespace detail
{

// `flatten_one<T>::type` is `std::tuple<T>` for a leaf type, and
// recursively flattens any `std::tuple<...>` wrapper.  The result of
// `flatten_t<Ts...>` is a single `std::tuple<...>` whose elements are
// the leaves of the input list, in left-to-right order.
template <typename T> struct flatten_one
{
    using type = std::tuple<T>;
};

template <typename... Ts> struct flatten_one<std::tuple<Ts...>>
{
    using type = decltype(std::tuple_cat(typename flatten_one<Ts>::type {}...));
};

template <typename... Ts> using flatten_t = decltype(std::tuple_cat(typename flatten_one<Ts>::type {}...));

template <typename Tuple> struct sort_fields;

// Validates that every element of the flattened tuple is a
// `field_generic_base` subclass, then computes the permutation that
// sorts them lexicographically by `(uint64_t(key), wire, original index)`.
// That ordering matches the strictly-ascending encoded-tag invariant
// downstream `schema<>` enforces.
//
// `to<Tmpl>` rebinds the sorted field pack onto an arbitrary
// `template <typename...> class` continuation.  The public
// `auto_schema` alias uses this to splice the sorted pack directly
// into `schema<...>` without exposing a `std::make_index_sequence`
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
