#include <ppb/ppb.hpp>

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
static_assert(sizeof(ppb::schema<ppb::varint<1>>) > 0);
static_assert(sizeof(ppb::schema<ppb::varint<1>, ppb::i64<2>>) > 0);
static_assert(sizeof(ppb::schema<ppb::varint<1>, ppb::i64<2>, ppb::len<3>, ppb::i32<4>>) > 0);
// Same field number, different wire types are distinct fields
// with distinct encoded tag bits, so ascending wire type order is valid.
static_assert(sizeof(ppb::schema<ppb::varint<1>, ppb::i64<1>>) > 0);
// proto3_zero_default is valid in any schema.
static_assert(sizeof(ppb::schema<ppb::int32<1, ppb::field_semantics::proto3_zero_default>>) > 0);
static_assert(sizeof(ppb::schema<ppb::int32<1, ppb::field_semantics::proto3_zero_default>,
                  ppb::uint64<2, ppb::field_semantics::last_write_wins>>) > 0);
static_assert(sizeof(ppb::schema<ppb::int32<1, ppb::field_semantics::proto3_zero_default>,
                  ppb::len<2, ppb::field_semantics::proto3_zero_default>>) > 0);
static_assert(sizeof(ppb::schema<ppb::int32<1, ppb::field_semantics::proto3_zero_default>,
                  ppb::varint<2, ppb::field_semantics::repeated>>) > 0);

// Schema public API: num_fields()
static_assert(ppb::schema<ppb::varint<1>>::num_fields() == 1);
static_assert(ppb::schema<ppb::varint<1>, ppb::i64<2>>::num_fields() == 2);
static_assert(ppb::schema<ppb::varint<1>, ppb::i64<2>, ppb::len<3>>::num_fields() == 3);

// Schema public API: s_encoded_tags
namespace test_encoded_tags
{
constexpr auto &tags = ppb::schema<ppb::varint<1>, ppb::i64<2>>::s_encoded_tags;
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
static_assert(sizeof(ppb::reader<ppb::schema<ppb::varint<1>>>) > 0);

// meta() before prescan: zero-filled metadata, demonstrable at compile time
namespace test_meta_before_prescan
{
constexpr ppb::reader<ppb::schema<ppb::varint<1>>> r;
constexpr ppb_field_meta m = r.meta<1>();
static_assert(m.num_occurrences == 0);
static_assert(m.total_bytes == 0);
static_assert(m.min_nonzero_bytes == 0);
static_assert(m.max_bytes == 0);
static_assert(m.lost_distinct_u64 == 0);
}  // namespace test_meta_before_prescan

// Merged schema (same key, different wire types): meta() is also zero before prescan.
namespace test_meta_merged_before_prescan
{
using Merged = ppb::schema<ppb::varint<1>, ppb::len<1>>;
constexpr ppb::reader<Merged> r;
constexpr ppb_field_meta m = r.meta<1>();
static_assert(m.num_occurrences == 0);
static_assert(m.total_bytes == 0);
static_assert(m.lost_distinct_u64 == 0);
}  // namespace test_meta_merged_before_prescan

// meta() with a key present in the schema (counterpart to PPB_FAIL_META_KEY_NOT_FOUND)
namespace test_meta_key_found
{
constexpr ppb_field_meta m = ppb::reader<ppb::schema<ppb::varint<1>>> {}.meta<1>();
static_assert(m.num_occurrences == 0);
}  // namespace test_meta_key_found

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
constexpr auto no_match = ppb::detail::find_value_handler<1, ppb::wire_type::varint, int>(
    std::tuple { ppb::on<2, ppb::wire_type::varint>(h_int) });
static_assert(!no_match.has_value());

// Exact key+wire match: returns the correct index.
constexpr auto exact = ppb::detail::find_value_handler<1, ppb::wire_type::varint, int>(
    std::tuple { ppb::on<2, ppb::wire_type::varint>(h_int), ppb::on<1, ppb::wire_type::varint>(h_int) });
static_assert(exact.has_value() && *exact == 1);

// wire_type::any matches any wire type for the key.
constexpr auto any_wire = ppb::detail::find_value_handler<1, ppb::wire_type::i64, int>(
    std::tuple { ppb::on<1>(h_int) });
static_assert(any_wire.has_value() && *any_wire == 0);

// Specific wire type does NOT match a different wire type.
constexpr auto wrong_wire = ppb::detail::find_value_handler<1, ppb::wire_type::i64, int>(
    std::tuple { ppb::on<1, ppb::wire_type::varint>(h_int) });
static_assert(!wrong_wire.has_value());

// Multiple key+wire matches, disambiguated by Arg invocability.
constexpr auto disambiguated = ppb::detail::find_value_handler<1, ppb::wire_type::varint, int>(
    std::tuple { ppb::on<1, ppb::wire_type::varint>(h_str), ppb::on<1, ppb::wire_type::varint>(h_int) });
static_assert(disambiguated.has_value() && *disambiguated == 1);

}  // namespace test_find_value_handler

// auto_schema: flatten + sort. Counterparts in PPB_FAIL_AUTO_SCHEMA_* tests.
namespace test_auto_schema
{

// Already-ordered single field is preserved.
static_assert(std::is_same_v<ppb::auto_schema<ppb::varint<1>>, ppb::schema<ppb::varint<1>>>);

// Out-of-order top-level fields are sorted.
static_assert(
    std::is_same_v<ppb::auto_schema<ppb::i64<2>, ppb::varint<1>>, ppb::schema<ppb::varint<1>, ppb::i64<2>>>);

// Same field number, different wire types: sorted by wire type.
static_assert(
    std::is_same_v<ppb::auto_schema<ppb::i64<1>, ppb::varint<1>>, ppb::schema<ppb::varint<1>, ppb::i64<1>>>);

// Single nested tuple is flattened.
static_assert(std::is_same_v<ppb::auto_schema<std::tuple<ppb::i64<2>, ppb::varint<1>>>,
    ppb::schema<ppb::varint<1>, ppb::i64<2>>>);

// Mix of leaf fields and tuples, including a deeply nested tuple.
static_assert(
    std::is_same_v<ppb::auto_schema<std::tuple<std::tuple<ppb::i64<2>>, ppb::varint<1>>, ppb::len<3>>,
        ppb::schema<ppb::varint<1>, ppb::i64<2>, ppb::len<3>>>);

// Empty nested tuples disappear; surrounding fields still sort.
static_assert(std::is_same_v<ppb::auto_schema<ppb::i32<4>, std::tuple<>, ppb::varint<1>>,
    ppb::schema<ppb::varint<1>, ppb::i32<4>>>);

// Typed scalars (subclasses of varint/i64/...) are accepted as fields.
static_assert(std::is_same_v<ppb::auto_schema<ppb::int32<2>, ppb::utf8string<1>>,
    ppb::schema<ppb::utf8string<1>, ppb::int32<2>>>);

}  // namespace test_auto_schema

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
static_assert(!ppb::varint<1>::is_unknown());
static_assert(!ppb::int32<1>::is_unknown());
static_assert(!ppb::field_generic_base::is_unknown());

// `auto_schema` flatten + sort places `detect_unknown_fields` at the tail
// of the schema in wire-type order.
using auto_with_catchalls = ppb::auto_schema<ppb::varint<1>, ppb::detect_unknown_fields>;
static_assert(auto_with_catchalls::num_fields() == 5);
// Tail four entries should be the catch-alls in varint/i64/len/i32 order.
static_assert(auto_with_catchalls::s_encoded_tags[1].bits == PPB_TAG_BITS(uint64_t(-1), 0));
static_assert(auto_with_catchalls::s_encoded_tags[2].bits == PPB_TAG_BITS(uint64_t(-1), 1));
static_assert(auto_with_catchalls::s_encoded_tags[3].bits == PPB_TAG_BITS(uint64_t(-1), 2));
static_assert(auto_with_catchalls::s_encoded_tags[4].bits == PPB_TAG_BITS(uint64_t(-1), 5));

// Manual schemas with catch-alls last (in wire-type order) also compile.
static_assert(
    sizeof(
        ppb::schema<ppb::varint<1>, ppb::unknown<ppb::wire_type::varint>, ppb::unknown<ppb::wire_type::i64>,
            ppb::unknown<ppb::wire_type::len>, ppb::unknown<ppb::wire_type::i32>>) > 0);

// Mixed enum-class Key types compose with the (Key-less) catch-alls.
namespace enum_key
{
enum class K : uint64_t
{
    one = 1,
    two = 2
};
using S = ppb::auto_schema<ppb::varint<K::one>, ppb::i64<K::two>, ppb::detect_unknown_fields>;
static_assert(S::num_fields() == 6);
static_assert(std::is_same_v<S::Key, K>);
}  // namespace enum_key

// `unknown<>` also accepts a non-default field_semantics.
static_assert(sizeof(ppb::unknown<ppb::wire_type::varint, ppb::field_semantics::last_write_wins>) > 0);

// Catch-all-only schemas compile: `key_of` skips fields without a
// `Key` typedef, so the schema resolves to the empty-pack default
// (`int`) and the same-Key-type check passes vacuously.
static_assert(sizeof(ppb::schema<ppb::unknown<ppb::wire_type::varint>>) > 0);
static_assert(sizeof(ppb::auto_schema<ppb::detect_unknown_fields>) > 0);

}  // namespace test_unknown_field

int
main()
{
    return 0;
}
