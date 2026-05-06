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

int
main()
{
    return 0;
}
