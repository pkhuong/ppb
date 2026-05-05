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

// Schema validation tests

/* expect-error: schema must include at least one field */
#ifdef PPB_FAIL_SCHEMA_EMPTY
static_assert(sizeof(ppb::schema<>) > 0);
#endif

/* expect-error: schema template arguments must be field_generic_base */
#ifdef PPB_FAIL_SCHEMA_NOT_A_FIELD
static_assert(sizeof(ppb::schema<int>) > 0);
#endif

/* expect-error: schema fields must all have the same Key type */
#ifdef PPB_FAIL_SCHEMA_DIFFERENT_KEYS
static_assert(sizeof(ppb::schema<ppb::varint<1>, ppb::varint<1L>>) > 0);
#endif

/* expect-error: schema fields must be listed in strictly ascending order */
#ifdef PPB_FAIL_SCHEMA_UNORDERED
static_assert(sizeof(ppb::schema<ppb::varint<2>, ppb::i64<1>>) > 0);
#endif

/* expect-error: schema fields must be listed in strictly ascending order */
#ifdef PPB_FAIL_SCHEMA_DUPLICATE_TAGS
static_assert(sizeof(ppb::schema<ppb::varint<1>, ppb::varint<1>>) > 0);
#endif

/* expect-error: schema fields must be listed in strictly ascending order */
#ifdef PPB_FAIL_SCHEMA_SAME_FIELD_WRONG_WIRE_ORDER
static_assert(sizeof(ppb::schema<ppb::i32<1>, ppb::varint<1>>) > 0);
#endif

// meta() with a key absent from the schema

/* expect-error: Key not found in schema */
#ifdef PPB_FAIL_META_KEY_NOT_FOUND
constexpr ppb_field_meta bad = ppb::reader<ppb::schema<ppb::varint<1>>> {}.meta<2>();
#endif

// meta() with a key present in the schema, but a wire type not associated with it

/* expect-error: Key not found in schema */
#ifdef PPB_FAIL_META_WIRE_TYPE_NOT_FOUND
constexpr ppb_field_meta bad = ppb::reader<ppb::schema<ppb::varint<1>>> {}.meta<1, ppb::wire_type::len>();
#endif
