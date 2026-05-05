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

int
main()
{
    return 0;
}
