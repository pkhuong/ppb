// Tests for compile-time diagnostics / rejections.
//
// Each block uses the format:
//   /* expect-error: <regex matched against compiler stderr> */
//   #ifdef PPB_FAIL_<NAME>
//   <code that should fail to compile>
//   #endif
//
// Matching positive tests in test_ppb_cpp_static.cc.

#include <ppb/ppb.hpp>

// Out-of-range key tests

/* expect-error: field tag key must be convertible to uint64_t */
#ifdef PPB_FAIL_KEY_ZERO
static_assert(sizeof(ppb::varint<0>) > 0);
#endif

/* expect-error: field tag key must be convertible to uint64_t */
#ifdef PPB_FAIL_KEY_2_POW_29
static_assert(sizeof(ppb::varint<(1 << 29)>) > 0);
#endif

/* expect-error: field tag key must be convertible to uint64_t */
#ifdef PPB_FAIL_KEY_INT64_MAX
static_assert(sizeof(ppb::varint<INT64_MAX>) > 0);
#endif

/* expect-error: field tag key must be convertible to uint64_t */
#ifdef PPB_FAIL_KEY_UINT64_MAX
static_assert(sizeof(ppb::varint<UINT64_MAX>) > 0);
#endif

/* expect-error: field tag key must be convertible to uint64_t */
#ifdef PPB_FAIL_KEY_NEG_ONE
static_assert(sizeof(ppb::varint<-1>) > 0);
#endif

// Invalid wire type tests

/* expect-error: field wire type must be one of varint, i64, len, or i32 */
#ifdef PPB_FAIL_WIRE_TYPE_3
static_assert(sizeof(ppb::field_base<1, static_cast<ppb::wire_type>(3)>) > 0);
#endif

/* expect-error: field wire type must be one of varint, i64, len, or i32 */
#ifdef PPB_FAIL_WIRE_TYPE_4
static_assert(sizeof(ppb::field_base<1, static_cast<ppb::wire_type>(4)>) > 0);
#endif

/* expect-error: field wire type must be one of varint, i64, len, or i32 */
#ifdef PPB_FAIL_WIRE_TYPE_6
static_assert(sizeof(ppb::field_base<1, static_cast<ppb::wire_type>(6)>) > 0);
#endif

/* expect-error: field wire type must be one of varint, i64, len, or i32 */
#ifdef PPB_FAIL_WIRE_TYPE_7
static_assert(sizeof(ppb::field_base<1, static_cast<ppb::wire_type>(7)>) > 0);
#endif

/* expect-error: field wire type must be one of varint, i64, len, or i32 */
#ifdef PPB_FAIL_WIRE_TYPE_255
static_assert(sizeof(ppb::field_base<1, static_cast<ppb::wire_type>(255)>) > 0);
#endif
