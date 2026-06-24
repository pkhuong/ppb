#include <ppb/ppb.hpp>
#include <vector>

namespace
{
enum class K : uint64_t
{
    f1 = 1,
    f2 = 2,
    f3 = 3,
    f4 = 4
};
}

/*
 * Positive static tests: these must all compile.
 * Each group has a matching negative test in test_ppb_cpp_compile_fail.cc.
 *
 * We use sizeof() to force template instantiation.
 */

// Boundary: minimum valid key (counterpart to PPB_FAIL_KEY_ZERO)
static_assert(sizeof(ppb::varint<1>) > 0);
static_assert(sizeof(ppb::i64<1>) > 0);
static_assert(sizeof(ppb::len<1>) > 0);
static_assert(sizeof(ppb::i32<1>) > 0);

// Boundary: maximum valid key, 2**29 - 1 (counterpart to PPB_FAIL_KEY_2_POW_29)
static_assert(sizeof(ppb::varint<(1 << 29) - 1>) > 0);
static_assert(sizeof(ppb::i64<(1 << 29) - 1>) > 0);
static_assert(sizeof(ppb::len<(1 << 29) - 1>) > 0);
static_assert(sizeof(ppb::i32<(1 << 29) - 1>) > 0);

// Mid-range valid keys (counterpart to PPB_FAIL_KEY_INT64_MAX / _UINT64_MAX / _NEG_ONE)
static_assert(sizeof(ppb::varint<42>) > 0);
static_assert(sizeof(ppb::i64<42>) > 0);
static_assert(sizeof(ppb::len<42>) > 0);
static_assert(sizeof(ppb::i32<42>) > 0);

// Wire type assertion passes for all four valid types (counterpart to _WIRE_TYPE_*)
static_assert(sizeof(ppb::field_base<1, ppb::wire_type::varint>) > 0);
static_assert(sizeof(ppb::field_base<1, ppb::wire_type::i64>) > 0);
static_assert(sizeof(ppb::field_base<1, ppb::wire_type::len>) > 0);
static_assert(sizeof(ppb::field_base<1, ppb::wire_type::i32>) > 0);

// Schema validation: valid schemas (counterpart to _SCHEMA_* negative tests)
static_assert(sizeof(ppb::schema<ppb::varint<K::f1>>) > 0);
static_assert(sizeof(ppb::schema<ppb::varint<K::f1>, ppb::i64<K::f2>>) > 0);
static_assert(sizeof(ppb::schema<ppb::varint<K::f1>, ppb::i64<K::f2>, ppb::len<K::f3>, ppb::i32<K::f4>>) > 0);
// Same field number, different wire types are distinct fields
// with distinct encoded tag bits, so ascending wire type order is valid.
static_assert(sizeof(ppb::schema<ppb::varint<K::f1>, ppb::i64<K::f1>>) > 0);
// proto3_zero_default is valid in any schema.
static_assert(sizeof(ppb::schema<ppb::int32<K::f1, ppb::field_semantics::proto3_zero_default>>) > 0);
static_assert(sizeof(ppb::schema<ppb::int32<K::f1, ppb::field_semantics::proto3_zero_default>,
                  ppb::uint64<K::f2, ppb::field_semantics::last_write_wins>>) > 0);
static_assert(sizeof(ppb::schema<ppb::int32<K::f1, ppb::field_semantics::proto3_zero_default>,
                  ppb::len<K::f2, ppb::field_semantics::proto3_zero_default>>) > 0);
static_assert(sizeof(ppb::schema<ppb::int32<K::f1, ppb::field_semantics::proto3_zero_default>,
                  ppb::varint<K::f2, ppb::field_semantics::repeated>>) > 0);

// proto3_* aliases are exactly the corresponding scalar with proto3_zero_default semantics.
namespace test_proto3_aliases
{
using enum ppb::field_semantics;

// Varint-backed
static_assert(std::is_same_v<ppb::proto3_int32<1>, ppb::int32<1, proto3_zero_default>>);
static_assert(std::is_same_v<ppb::proto3_int64<2>, ppb::int64<2, proto3_zero_default>>);
static_assert(std::is_same_v<ppb::proto3_sint32<3>, ppb::sint32<3, proto3_zero_default>>);
static_assert(std::is_same_v<ppb::proto3_sint64<4>, ppb::sint64<4, proto3_zero_default>>);
static_assert(std::is_same_v<ppb::proto3_uint32<5>, ppb::uint32<5, proto3_zero_default>>);
static_assert(std::is_same_v<ppb::proto3_uint64<6>, ppb::uint64<6, proto3_zero_default>>);
static_assert(std::is_same_v<ppb::proto3_boolean<7>, ppb::boolean<7, proto3_zero_default>>);

// Varint + enum
enum class test_enum : int
{
    a = 0,
    b = 1
};
static_assert(
    std::is_same_v<ppb::proto3_enumerated<8, test_enum>, ppb::enumerated<8, test_enum, proto3_zero_default>>);

// counterpart to PPB_FAIL_ENUMERATED_NOT_AN_ENUM
static_assert(sizeof(ppb::enumerated<9, test_enum>) > 0);

// counterpart to PPB_FAIL_PACKED_ENUMERATED_NOT_AN_ENUM
static_assert(sizeof(ppb::packed_enumerated<10, test_enum>) > 0);

// I32-backed
static_assert(std::is_same_v<ppb::proto3_fixed32<9>, ppb::fixed32<9, proto3_zero_default>>);
static_assert(std::is_same_v<ppb::proto3_sfixed32<10>, ppb::sfixed32<10, proto3_zero_default>>);
static_assert(std::is_same_v<ppb::proto3_f32<11>, ppb::f32<11, proto3_zero_default>>);

// I64-backed
static_assert(std::is_same_v<ppb::proto3_fixed64<12>, ppb::fixed64<12, proto3_zero_default>>);
static_assert(std::is_same_v<ppb::proto3_sfixed64<13>, ppb::sfixed64<13, proto3_zero_default>>);
static_assert(std::is_same_v<ppb::proto3_f64<14>, ppb::f64<14, proto3_zero_default>>);

// LEN-backed
static_assert(std::is_same_v<ppb::proto3_utf8string<15>, ppb::utf8string<15, proto3_zero_default>>);
static_assert(std::is_same_v<ppb::proto3_bytes<16>, ppb::bytes<16, std::byte, proto3_zero_default>>);
static_assert(std::is_same_v<ppb::proto3_bytes<17, char>, ppb::bytes<17, char, proto3_zero_default>>);
}  // namespace test_proto3_aliases

// Schema public API: num_fields()
static_assert(ppb::schema<ppb::varint<K::f1>>::num_fields() == 1);
static_assert(ppb::schema<ppb::varint<K::f1>, ppb::i64<K::f2>>::num_fields() == 2);
static_assert(ppb::schema<ppb::varint<K::f1>, ppb::i64<K::f2>, ppb::len<K::f3>>::num_fields() == 3);

// Schema public API: s_encoded_tags
namespace test_encoded_tags
{
constexpr auto &tags = ppb::schema<ppb::varint<K::f1>, ppb::i64<K::f2>>::s_encoded_tags;
static_assert(tags.size() == 2);
static_assert(tags[0].bits == PPB_TAG_BITS(1, PPB_WIRE_VARINT));
static_assert(tags[1].bits == PPB_TAG_BITS(2, PPB_WIRE_I64));
}  // namespace test_encoded_tags

// limit factories are constexpr
static_assert(ppb::limit::max_fields(5).fields() == 5);
static_assert(ppb::limit::hard(100).bytes() == 100);
static_assert(ppb::limit::hard(100).error_on_bytes() == PPB_ERROR_LIMIT_EXCEEDED);
static_assert(ppb::limit::soft(100).error_on_bytes() == PPB_OK);

// reader is instantiable with valid schemas
static_assert(sizeof(ppb::reader<ppb::schema<ppb::varint<K::f1>>>) > 0);

// meta() before prescan: zero-filled metadata, demonstrable at compile time
namespace test_meta_before_prescan
{
constexpr ppb::reader<ppb::schema<ppb::varint<K::f1>>> r;
constexpr ppb_field_meta m = r.meta<K::f1>();
static_assert(m.num_occurrences == 0);
static_assert(m.total_bytes == 0);
static_assert(m.min_nonzero_bytes == 0);
static_assert(m.max_bytes == 0);
static_assert(m.lost_distinct_u64 == 0);
}  // namespace test_meta_before_prescan

// Merged schema (same key, different wire types): meta() is also zero before prescan.
namespace test_meta_merged_before_prescan
{
using Merged = ppb::schema<ppb::varint<K::f1>, ppb::len<K::f1>>;
constexpr ppb::reader<Merged> r;
constexpr ppb_field_meta m = r.meta<K::f1>();
static_assert(m.num_occurrences == 0);
static_assert(m.total_bytes == 0);
static_assert(m.lost_distinct_u64 == 0);
}  // namespace test_meta_merged_before_prescan

// meta() with a key present in the schema (counterpart to PPB_FAIL_META_KEY_NOT_FOUND)
namespace test_meta_key_found
{
constexpr ppb_field_meta m = ppb::reader<ppb::schema<ppb::varint<K::f1>>> {}.meta<K::f1>();
static_assert(m.num_occurrences == 0);
}  // namespace test_meta_key_found

// field() with a key present (single wire, default any) — counterpart to PPB_FAIL_FIELD_KEY_NOT_FOUND
namespace test_field_key_found
{
constexpr ppb::reader<ppb::schema<ppb::varint<K::f1>>> r;
static_assert(r.field<K::f1>().m.num_occurrences == 0);
}  // namespace test_field_key_found

// field() with explicit wire selection — counterpart to PPB_FAIL_FIELD_WIRE_TYPE_NOT_FOUND
namespace test_field_wire_found
{
constexpr ppb::reader<ppb::schema<ppb::varint<K::f1>>> r;
static_assert(r.field<K::f1, ppb::wire_type::varint>().m.num_occurrences == 0);
}  // namespace test_field_wire_found

// field() with wire that disambiguates a multi-wire key — counterpart to PPB_FAIL_FIELD_AMBIGUOUS
namespace test_field_wire_disambiguates
{
using Merged = ppb::schema<ppb::varint<K::f1>, ppb::len<K::f1>>;
constexpr ppb::reader<Merged> r;
static_assert(r.field<K::f1, ppb::wire_type::varint>().m.num_occurrences == 0);
static_assert(r.field<K::f1, ppb::wire_type::len>().m.num_occurrences == 0);
}  // namespace test_field_wire_disambiguates

// dispatch(): compile-time dispatch to handlers from begin up to end (or limit).
namespace test_dispatch
{
// Helper: run dispatch and return how many times the handler was called.
constexpr size_t
count_visits(size_t begin)
{
    size_t n = 0;

    ppb::detail::dispatch<5>(begin, 5,
        [&n]<size_t I>(std::integral_constant<size_t, I>)
        {
            n++;
            return true;
        });

    return n;
}

static_assert(count_visits(0) == 5);  // begin=0, limit=5
static_assert(count_visits(3) == 2);  // begin=3, limit=5
static_assert(count_visits(5) == 0);  // begin == limit
static_assert(count_visits(10) == 0); // begin > limit

// Early exit: handler returns false, dispatch returns false, stops after first.
constexpr bool early_exit = []
{
    int n = 0;
    bool ok = ppb::detail::dispatch<10>(0, 10,
        [&n]<size_t I>(std::integral_constant<size_t, I>)
        {
            n++;
            return false;
        });

    return !ok && n == 1;
}();
static_assert(early_exit);

// Cross-block boundary: 32 indices broken into 2 blocks of 16.  With
// begin=15, the first block visits index 15 only, then block 2 visits 16..31.
constexpr size_t cross_block = []
{
    size_t n = 0;

    ppb::detail::dispatch<32>(15, 32,
        [&n]<size_t I>(std::integral_constant<size_t, I>)
        {
            n++;
            return true;
        });

    return n;
}();
static_assert(cross_block == 17);

// Indices are visited in ascending order, starting at begin, even across
// block boundaries.  We capture the sequence into an array and verify.
constexpr bool in_order = []
{
    std::array<size_t, 24> seq {};
    size_t pos = 0;

    ppb::detail::dispatch<24>(5, 24,
        [&]<size_t I>(std::integral_constant<size_t, I>)
        {
            seq[pos++] = I;
            return true;
        });

    for (size_t i = 0; i < 19; i++)
    {
        if (seq[i] != 5 + i)
            return false;
    }

    return pos == 19;  // 5..23 inclusive
}();
static_assert(in_order);

// begin > 16: the first block is entirely skipped.
constexpr size_t begin_past_first_block = []
{
    size_t n = 0;

    ppb::detail::dispatch<40>(20, 40,
        [&n]<size_t I>(std::integral_constant<size_t, I>)
        {
            n++;
            return true;
        });

    return n;
}();
static_assert(begin_past_first_block == 20);  // 20..39 inclusive

// Half-open `end`: stops at end-1 even when below the static limit.
constexpr bool end_within_block = []
{
    std::array<size_t, 16> seq {};
    size_t pos = 0;

    ppb::detail::dispatch<16>(2, 7,
        [&]<size_t I>(std::integral_constant<size_t, I>)
        {
            seq[pos++] = I;
            return true;
        });

    if (pos != 5)
        return false;

    for (size_t i = 0; i < 5; i++)
    {
        if (seq[i] != 2 + i)
            return false;
    }

    return true;
}();
static_assert(end_within_block);

// `end` straddling a block boundary: visits across blocks, then halts.
constexpr size_t end_cross_block = []
{
    size_t n = 0;

    ppb::detail::dispatch<48>(10, 20,
        [&n]<size_t I>(std::integral_constant<size_t, I>)
        {
            n++;
            return true;
        });

    return n;  // 10..19 inclusive
}();
static_assert(end_cross_block == 10);

// `begin >= end`: no visits, regardless of how large `limit` is.
static_assert(
    []
    {
        size_t n = 0;

        ppb::detail::dispatch<32>(7, 7,
            [&n]<size_t I>(std::integral_constant<size_t, I>)
            {
                n++;
                return true;
            });

        return n;
    }() == 0);

}  // namespace test_dispatch

// find_value_handler(): select the value_handler matching (Key, wire, Arg).
namespace test_find_value_handler
{
// Dummy handler function pointers.
using H_int = void (*)(int);
using H_str = void (*)(const char *);

constexpr H_int h_int = nullptr;
constexpr H_str h_str = nullptr;

// No match: returns nullopt.
constexpr auto no_match = ppb::detail::find_value_handler<K::f1, ppb::wire_type::varint, int>(
    std::tuple { ppb::on<K::f2, ppb::wire_type::varint>(h_int) });
static_assert(!no_match.has_value());

// Exact key+wire match: returns the correct index.
constexpr auto exact = ppb::detail::find_value_handler<K::f1, ppb::wire_type::varint, int>(std::tuple {
    ppb::on<K::f2, ppb::wire_type::varint>(h_int), ppb::on<K::f1, ppb::wire_type::varint>(h_int) });
static_assert(exact.has_value() && exact.value() == 1);

// wire_type::any matches any wire type for the key.
constexpr auto any_wire = ppb::detail::find_value_handler<K::f1, ppb::wire_type::i64, int>(
    std::tuple { ppb::on<K::f1>(h_int) });
static_assert(any_wire.has_value() && any_wire.value() == 0);

// Specific wire type does NOT match a different wire type.
constexpr auto wrong_wire = ppb::detail::find_value_handler<K::f1, ppb::wire_type::i64, int>(
    std::tuple { ppb::on<K::f1, ppb::wire_type::varint>(h_int) });
static_assert(!wrong_wire.has_value());

// Multiple key+wire matches, disambiguated by Arg invocability.
constexpr auto disambiguated = ppb::detail::find_value_handler<K::f1, ppb::wire_type::varint, int>(
    std::tuple { ppb::on<K::f1, ppb::wire_type::varint>(h_str),
        ppb::on<K::f1, ppb::wire_type::varint>(h_int) });
static_assert(disambiguated.has_value() && disambiguated.value() == 1);

}  // namespace test_find_value_handler

// auto_schema: flatten + sort. Counterparts in PPB_FAIL_AUTO_SCHEMA_* tests.
namespace test_auto_schema
{

// Already-ordered single field is preserved.
static_assert(std::is_same_v<ppb::auto_schema<ppb::varint<K::f1>>, ppb::schema<ppb::varint<K::f1>>>);

// Out-of-order top-level fields are sorted.
static_assert(std::is_same_v<ppb::auto_schema<ppb::i64<K::f2>, ppb::varint<K::f1>>,
    ppb::schema<ppb::varint<K::f1>, ppb::i64<K::f2>>>);

// Same field number, different wire types: sorted by wire type.
static_assert(std::is_same_v<ppb::auto_schema<ppb::i64<K::f1>, ppb::varint<K::f1>>,
    ppb::schema<ppb::varint<K::f1>, ppb::i64<K::f1>>>);

// Single nested tuple is flattened.
static_assert(std::is_same_v<ppb::auto_schema<std::tuple<ppb::i64<K::f2>, ppb::varint<K::f1>>>,
    ppb::schema<ppb::varint<K::f1>, ppb::i64<K::f2>>>);

// Mix of leaf fields and tuples, including a deeply nested tuple.
static_assert(std::is_same_v<
    ppb::auto_schema<std::tuple<std::tuple<ppb::i64<K::f2>>, ppb::varint<K::f1>>, ppb::len<K::f3>>,
    ppb::schema<ppb::varint<K::f1>, ppb::i64<K::f2>, ppb::len<K::f3>>>);

// Empty nested tuples disappear; surrounding fields still sort.
static_assert(std::is_same_v<ppb::auto_schema<ppb::i32<K::f4>, std::tuple<>, ppb::varint<K::f1>>,
    ppb::schema<ppb::varint<K::f1>, ppb::i32<K::f4>>>);

// Typed scalars (subclasses of varint/i64/...) are accepted as fields.
static_assert(std::is_same_v<ppb::auto_schema<ppb::int32<K::f2>, ppb::utf8string<K::f1>>,
    ppb::schema<ppb::utf8string<K::f1>, ppb::int32<K::f2>>>);

}  // namespace test_auto_schema

// auto_schema splices a complete schema and re-sorts its fields with
// the rest, rather than treating it as an opaque leaf.
namespace test_auto_schema_flatten
{
using inner = ppb::schema<ppb::varint<K::f1>, ppb::i32<K::f3>>;
static_assert(std::is_same_v<ppb::auto_schema<ppb::varint<K::f2>, inner>,
    ppb::schema<ppb::varint<K::f1>, ppb::varint<K::f2>, ppb::i32<K::f3>>>);
}  // namespace test_auto_schema_flatten

// An unknown-only catch-all is a schema in its own right: usable as a
// reader schema without wrapping in auto_schema.
namespace test_detect_unknown_is_schema
{
using S = ppb::detect_unknown_fields<>;
static_assert(sizeof(ppb::reader<S>) > 0, "reader over a bare detect_unknown_fields");
}  // namespace test_detect_unknown_is_schema

// Catch-all / unknown<> field type. Negative counterpart in
// PPB_FAIL_UNKNOWN_*.
namespace test_unknown_field
{

// All four wire types instantiate.
static_assert(sizeof(ppb::unknown<ppb::wire_type::varint>) > 0);
static_assert(sizeof(ppb::unknown<ppb::wire_type::i64>) > 0);
static_assert(sizeof(ppb::unknown<ppb::wire_type::len>) > 0);
static_assert(sizeof(ppb::unknown<ppb::wire_type::i32>) > 0);

// Catch-all encoded tags match the C-side `PPB_TAG_BITS(-1, wire)`.
static_assert(ppb::unknown<ppb::wire_type::varint>::encoded_tag().bits == PPB_TAG_BITS(uint64_t(-1), 0));
static_assert(ppb::unknown<ppb::wire_type::i64>::encoded_tag().bits == PPB_TAG_BITS(uint64_t(-1), 1));
static_assert(ppb::unknown<ppb::wire_type::len>::encoded_tag().bits == PPB_TAG_BITS(uint64_t(-1), 2));
static_assert(ppb::unknown<ppb::wire_type::i32>::encoded_tag().bits == PPB_TAG_BITS(uint64_t(-1), 5));

// `is_unknown()` returns true on catch-alls, false on real fields.
static_assert(ppb::unknown<ppb::wire_type::varint>::is_unknown());
static_assert(!ppb::varint<K::f1>::is_unknown());
static_assert(!ppb::int32<K::f1>::is_unknown());
static_assert(!ppb::field_generic_base::is_unknown());

// `auto_schema` flatten + sort places `detect_unknown_fields` at the tail
// of the schema in wire-type order.
using auto_with_catchalls = ppb::auto_schema<ppb::varint<K::f1>, ppb::detect_unknown_fields<>>;
static_assert(auto_with_catchalls::num_fields() == 5);
// Tail four entries should be the catch-alls in varint/i64/len/i32 order.
static_assert(auto_with_catchalls::s_encoded_tags[1].bits == PPB_TAG_BITS(uint64_t(-1), 0));
static_assert(auto_with_catchalls::s_encoded_tags[2].bits == PPB_TAG_BITS(uint64_t(-1), 1));
static_assert(auto_with_catchalls::s_encoded_tags[3].bits == PPB_TAG_BITS(uint64_t(-1), 2));
static_assert(auto_with_catchalls::s_encoded_tags[4].bits == PPB_TAG_BITS(uint64_t(-1), 5));

// Manual schemas with catch-alls last (in wire-type order) also compile.
static_assert(sizeof(ppb::schema<ppb::varint<K::f1>, ppb::unknown<ppb::wire_type::varint>,
                  ppb::unknown<ppb::wire_type::i64>, ppb::unknown<ppb::wire_type::len>,
                  ppb::unknown<ppb::wire_type::i32>>) > 0);

// Mixed enum-class Key types compose with the (Key-less) catch-alls.
namespace enum_key
{
enum class K : uint64_t
{
    one = 1,
    two = 2
};
using S = ppb::auto_schema<ppb::varint<K::one>, ppb::i64<K::two>, ppb::detect_unknown_fields<>>;
static_assert(S::num_fields() == 6);
static_assert(std::is_same_v<S::Key, K>);
}  // namespace enum_key

// `unknown<>` also accepts a non-default field_semantics.
static_assert(sizeof(ppb::unknown<ppb::wire_type::varint, ppb::field_semantics::last_write_wins>) > 0);

// `detect_unknown_fields` with non-default semantics.
static_assert(sizeof(ppb::auto_schema<ppb::detect_unknown_fields<ppb::field_semantics::error>>) > 0);
static_assert(
    sizeof(ppb::auto_schema<ppb::detect_unknown_fields<ppb::field_semantics::last_write_wins>>) > 0);

// Catch-all-only schemas compile: `key_of` skips fields without a
// `Key` typedef, so the schema resolves to the empty-pack default
// (`int`) and the same-Key-type check passes vacuously.
static_assert(sizeof(ppb::schema<ppb::unknown<ppb::wire_type::varint>>) > 0);
static_assert(sizeof(ppb::auto_schema<ppb::detect_unknown_fields<>>) > 0);

}  // namespace test_unknown_field

// `ppb::on_unknown` factory: produces an `unknown_handler` and
// composes with `find_unknown_handler`.  Negative counterparts in
// PPB_FAIL_ON_UNKNOWN_*.
namespace test_on_unknown
{

constexpr auto h = ppb::on_unknown([](const ppb_field &) -> ppb_error { return PPB_OK; });

// `unknown_handler` carries the catch-all marker; not a value_handler.
static_assert(decltype(h)::is_unknown_handler());

// Default wire = `any`; both varint and i32 dispatch through it.
constexpr auto found_varint =
    ppb::detail::find_unknown_handler<ppb::wire_type::varint, const ppb_field &, decltype(h)>();
static_assert(found_varint.has_value() && found_varint.value() == 0);

constexpr auto found_i32 =
    ppb::detail::find_unknown_handler<ppb::wire_type::i32, const ppb_field &, decltype(h)>();
static_assert(found_i32.has_value() && found_i32.value() == 0);

// Wire-pinned handler only matches that wire.
constexpr auto h_varint = ppb::on_unknown<ppb::wire_type::varint>(
    [](const ppb_field &) -> ppb_error { return PPB_OK; });

constexpr auto found_specific =
    ppb::detail::find_unknown_handler<ppb::wire_type::varint, const ppb_field &, decltype(h_varint)>();
static_assert(found_specific.has_value() && found_specific.value() == 0);

constexpr auto missed =
    ppb::detail::find_unknown_handler<ppb::wire_type::i32, const ppb_field &, decltype(h_varint)>();
static_assert(!missed.has_value());

// `find_value_handler` ignores `unknown_handler`s mixed into the pack.
constexpr auto value_only = ppb::detail::find_value_handler<K::f1, ppb::wire_type::varint, int,
    ppb::detail::value_handler<K::f1, ppb::wire_type::varint, int (*)(int)>, decltype(h)>();
static_assert(value_only.has_value() && value_only.value() == 0);

}  // namespace test_on_unknown

// handler_matches_some_field: positive cases (counterpart to
// PPB_FAIL_RUN_HANDLERS_* tests).
namespace test_handler_matches_some_field
{

using Hf = ppb_error (*)(const ppb_field &);
constexpr Hf hf = nullptr;

// on<Key, specific_wire> matches a field with that key and wire.
using H_varint = decltype(ppb::on<K::f1, ppb::wire_type::varint>(hf));
static_assert(ppb::detail::handler_matches_some_field<H_varint, ppb::varint<K::f1>>());
static_assert(ppb::detail::handler_matches_some_field<H_varint, ppb::varint<K::f1>, ppb::i64<K::f1>>());
// Does not match when the only field has the same key but a different wire.
static_assert(!ppb::detail::handler_matches_some_field<H_varint, ppb::i64<K::f1>>());
// Does not match when the key is absent.
static_assert(!ppb::detail::handler_matches_some_field<H_varint, ppb::varint<K::f2>>());

// on<Key> with wire_type::any matches any wire type for that key.
using H_any = decltype(ppb::on<K::f1>(hf));
static_assert(ppb::detail::handler_matches_some_field<H_any, ppb::varint<K::f1>>());
static_assert(ppb::detail::handler_matches_some_field<H_any, ppb::i64<K::f1>>());
static_assert(ppb::detail::handler_matches_some_field<H_any, ppb::len<K::f1>>());

// on_unknown<any> matches any ppb::unknown entry.
using HU = decltype(ppb::on_unknown(hf));
static_assert(
    ppb::detail::handler_matches_some_field<HU, ppb::varint<K::f1>, ppb::unknown<ppb::wire_type::varint>>());
static_assert(
    ppb::detail::handler_matches_some_field<HU, ppb::varint<K::f1>, ppb::unknown<ppb::wire_type::i64>>());
static_assert(
    ppb::detail::handler_matches_some_field<HU, ppb::varint<K::f1>, ppb::unknown<ppb::wire_type::len>>());
static_assert(
    ppb::detail::handler_matches_some_field<HU, ppb::varint<K::f1>, ppb::unknown<ppb::wire_type::i32>>());
// Does not match when there is no catch-all.
static_assert(!ppb::detail::handler_matches_some_field<HU, ppb::varint<K::f1>>());

// on_unknown<wire> only matches catch-alls with that wire type.
using HU_varint = decltype(ppb::on_unknown<ppb::wire_type::varint>(hf));
static_assert(ppb::detail::handler_matches_some_field<HU_varint, ppb::varint<K::f1>,
    ppb::unknown<ppb::wire_type::varint>>());
static_assert(!ppb::detail::handler_matches_some_field<HU_varint, ppb::unknown<ppb::wire_type::i64>>());

}  // namespace test_handler_matches_some_field

// `ppb::store`, `ppb::push_back`, `ppb::emplace_back` produce the same
// value_handler shape as `ppb::on` and compose with existing dispatch.
namespace test_handler_factories
{

static int dest = 0;
static std::vector<int> vec;

using H_store = decltype(ppb::store<K::f1>(&dest));
using H_push = decltype(ppb::push_back<K::f1>(&vec));
using H_emplace = decltype(ppb::emplace_back<K::f1>(&vec));

// All three are value_handlers, not unknown_handlers.
static_assert(!H_store::is_unknown_handler());
static_assert(!H_push::is_unknown_handler());
static_assert(!H_emplace::is_unknown_handler());

// Default wire = any; key = 1.
static_assert(H_store::key() == K::f1);
static_assert(H_store::wire() == ppb::wire_type::any);

// find_value_handler resolves them by (Key, wire, argument type).
constexpr auto s = ppb::detail::find_value_handler<K::f1, ppb::wire_type::any, int, H_store>();
static_assert(s.has_value() && s.value() == 0);

constexpr auto p = ppb::detail::find_value_handler<K::f1, ppb::wire_type::any, int, H_push>();
static_assert(p.has_value() && p.value() == 0);

constexpr auto e = ppb::detail::find_value_handler<K::f1, ppb::wire_type::any, int, H_emplace>();
static_assert(e.has_value() && e.value() == 0);

// handler_matches_some_field: store matches int32 for key 1.
static_assert(ppb::detail::handler_matches_some_field<H_store, ppb::int32<K::f1>>());
static_assert(ppb::detail::handler_matches_some_field<H_push, ppb::unpacked_int32<K::f1>>());
static_assert(ppb::detail::handler_matches_some_field<H_emplace, ppb::unpacked_int32<K::f1>>());

// Key mismatch: store<K::f1> does not match key 2 fields.
static_assert(!ppb::detail::handler_matches_some_field<H_store, ppb::int32<K::f2>>());

}  // namespace test_handler_factories

namespace test_on_submessage
{
using Inner = ppb::schema<ppb::varint<K::f1>>;

constexpr auto inner_h = ppb::on<K::f1>([](const ppb_field &) -> ppb_error { return PPB_OK; });
constexpr auto subh = ppb::on_submessage<K::f3, Inner>(inner_h);
using H_sub = decltype(subh);

// `on_submessage` produces a handler (inherits from handler_base via
// value_handler).
static_assert(ppb::detail::is_handler_v<H_sub>);

// It identifies as a submessage handler.
static_assert(H_sub::is_submessage_handler());

// Plain `on` and `on_unknown` handlers do not.
static_assert(!decltype(ppb::on<K::f1>(inner_h.handler))::is_submessage_handler());
static_assert(!decltype(ppb::on_unknown<ppb::wire_type::varint>(
    [](const ppb_field &) -> ppb_error { return PPB_OK; }))::is_submessage_handler());

// Wire is len; key carries through.
static_assert(H_sub::wire() == ppb::wire_type::len);
static_assert(H_sub::key() == K::f3);

// `find_value_handler` matches it for a LEN-wire field at the same key,
// against any of the LEN extract_value argument types.
constexpr auto found_bytes =
    ppb::detail::find_value_handler<K::f3, ppb::wire_type::len, std::span<const std::byte>, H_sub>();
static_assert(found_bytes.has_value() && found_bytes.value() == 0);

constexpr auto found_strv =
    ppb::detail::find_value_handler<K::f3, ppb::wire_type::len, std::string_view, H_sub>();
static_assert(found_strv.has_value() && found_strv.value() == 0);

constexpr auto found_field =
    ppb::detail::find_value_handler<K::f3, ppb::wire_type::len, const ppb_field &, H_sub>();
static_assert(found_field.has_value() && found_field.value() == 0);

// Wrong wire (varint) yields no match.
constexpr auto no_wire =
    ppb::detail::find_value_handler<K::f3, ppb::wire_type::varint, std::span<const std::byte>, H_sub>();
static_assert(!no_wire.has_value());

// Wrong key yields no match.
constexpr auto no_key =
    ppb::detail::find_value_handler<K::f4, ppb::wire_type::len, std::span<const std::byte>, H_sub>();
static_assert(!no_key.has_value());

// `handler_matches_some_field` accepts any LEN-wire field at the
// matching key, including `ppb::message<K, S>`.
static_assert(ppb::detail::handler_matches_some_field<H_sub, ppb::bytes<K::f3>>());
static_assert(ppb::detail::handler_matches_some_field<H_sub, ppb::utf8string<K::f3>>());
static_assert(ppb::detail::handler_matches_some_field<H_sub, ppb::len<K::f3>>());
static_assert(ppb::detail::handler_matches_some_field<H_sub, ppb::unpacked_bytes<K::f3>>());
static_assert(ppb::detail::handler_matches_some_field<H_sub, ppb::message<K::f3, Inner>>());

// `is_message_field_v` only fires for `ppb::message<K, S>` and aliases.
static_assert(ppb::detail::is_message_field_v<ppb::message<K::f1, Inner>>);
static_assert(ppb::detail::is_message_field_v<ppb::unpacked_message<K::f1, Inner>>);
static_assert(!ppb::detail::is_message_field_v<ppb::bytes<K::f1>>);
static_assert(!ppb::detail::is_message_field_v<ppb::utf8string<K::f1>>);
static_assert(!ppb::detail::is_message_field_v<ppb::len<K::f1>>);
static_assert(!ppb::detail::is_message_field_v<ppb::varint<K::f1>>);

// `inner_schema` typedef on `ppb::message<K, S>` round-trips.
static_assert(std::is_same_v<typename ppb::message<K::f1, Inner>::inner_schema, Inner>);
static_assert(std::is_same_v<typename ppb::unpacked_message<K::f1, Inner>::inner_schema, Inner>);

// Convenience aliases pin the expected semantics.
static_assert(ppb::message<K::f1, Inner>::semantics() == ppb::field_semantics::last_write_wins);
static_assert(ppb::unpacked_message<K::f1, Inner>::semantics() == ppb::field_semantics::repeated);

}  // namespace test_on_submessage

// Positive counterpart to PPB_FAIL_MESSAGE_PROTO3:
// message<> with last_write_wins (the default) and repeated semantics both compile.
namespace test_message_valid_semantics
{
using Inner = ppb::schema<ppb::varint<K::f1>>;

static_assert(sizeof(ppb::message<K::f1, Inner>) > 0);
static_assert(ppb::message<K::f1, Inner>::semantics() == ppb::field_semantics::last_write_wins);
static_assert(sizeof(ppb::message<K::f1, Inner, ppb::field_semantics::repeated>) > 0);
}  // namespace test_message_valid_semantics

// Positive counterpart to PPB_FAIL_ON_SUBMESSAGE_PROTO3_FIELD:
// on_submessage with a last_write_wins message field compiles through
// run_handler_for_idx (the prescan overload triggers the full path).
namespace test_on_submessage_non_proto3_field
{
using Inner = ppb::schema<ppb::varint<K::f1>>;
using Outer = ppb::schema<ppb::message<K::f1, Inner>>;

void
trigger()
{
    (void)trigger;

    (void)ppb::reader<Outer>().prescan({},
        ppb::on_submessage<K::f1, Inner>(
            ppb::on<K::f1>([](const ppb_field &) -> ppb_error { return PPB_OK; })));
}
}  // namespace test_on_submessage_non_proto3_field

namespace test_limit_max_depth
{
// Default: depth budget is zero (fail-closed).
static_assert(ppb::limit {}.depth() == 0);

// Builder + factory round-trip.
static_assert(ppb::limit {}.with_max_depth(7).depth() == 7);
static_assert(ppb::limit::max_depth(5).depth() == 5);

// Other limit fields are untouched by max_depth.
static_assert(ppb::limit::max_depth(3).fields() == ppb::limit {}.fields());
static_assert(ppb::limit::max_depth(3).bytes() == ppb::limit {}.bytes());

}  // namespace test_limit_max_depth

int
main()
{
    return 0;
}
