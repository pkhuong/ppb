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

int
main()
{
    return 0;
}
