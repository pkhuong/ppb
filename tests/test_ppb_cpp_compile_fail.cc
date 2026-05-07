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

// find_value_handler: multiple key+wire matches, none invocable with Arg
//
// Both handlers match key=1, varint, but neither accepts `long`.

/* expect-error: value_handlers match \(Key, wire\), but none is invocable with the argument type */
#ifdef PPB_FAIL_FIND_VALUE_HANDLER_NONE_INVOCABLE
struct Foo
{
};
constexpr auto bad = ppb::detail::find_value_handler<1, ppb::wire_type::varint, Foo>(
    std::tuple { ppb::on<1, ppb::wire_type::varint>([](int) { }),
        ppb::on<1, ppb::wire_type::varint>([](const char *) { }) });
#endif

// find_value_handler: multiple key+wire matches, multiple invocable with Arg
//
// Both handlers match key=1, varint, and both accept `int`: ambiguous call rejected.

/* expect-error: multiple value_handlers match \(Key, wire\) and accept the argument type; ambiguous */
#ifdef PPB_FAIL_FIND_VALUE_HANDLER_AMBIGUOUS
constexpr auto bad = ppb::detail::find_value_handler<1, ppb::wire_type::varint, int>(std::tuple {
    ppb::on<1, ppb::wire_type::varint>([](int) { }), ppb::on<1, ppb::wire_type::varint>([](int) { }) });
#endif

// find_value_handler: handler key type differs from the dispatch Key type
//
// Dispatch Key is the `int` literal `1`; the handler is built with the
// `long` literal `1L`, so the all-same-key-type static_assert fires.

/* expect-error: every value_handler in the tuple must use the same key type as the dispatch Key */
#ifdef PPB_FAIL_FIND_VALUE_HANDLER_KEY_TYPE_MISMATCH
constexpr auto bad = ppb::detail::find_value_handler<1, ppb::wire_type::varint, int>(
    std::tuple { ppb::on<1L, ppb::wire_type::varint>([](int) { }) });
#endif

// Same static_asserts exercised through the prescan() call path.

static const uint8_t smoke_wire[] = { 0x08, 0x01 };  // field 1 varint 1
using SmokeSchema = ppb::schema<ppb::varint<1>>;

/* expect-error: multiple value_handlers match \(Key, wire\) and accept the argument type; ambiguous */
#ifdef PPB_FAIL_PRESCAN_AMBIGUOUS_HANDLERS
constexpr auto bad = ppb::reader<SmokeSchema>(smoke_wire, sizeof(smoke_wire))
                         .prescan({}, ppb::on<1>([](const ppb_field &) -> ppb_error { return PPB_OK; }),
                             ppb::on<1>([](const ppb_field &) -> ppb_error { return PPB_OK; }));
#endif

/* expect-error: value_handlers match \(Key, wire\), but none is invocable with the argument type */
#ifdef PPB_FAIL_PRESCAN_NONE_INVOCABLE
constexpr auto bad = ppb::reader<SmokeSchema>(smoke_wire, sizeof(smoke_wire))
                         .prescan({},
                             ppb::on<1, ppb::wire_type::varint>([](int) -> ppb_error { return PPB_OK; }),
                             ppb::on<1, ppb::wire_type::varint>(
                                 [](const char *) -> ppb_error { return PPB_OK; }));
#endif

/* expect-error: every value_handler in the tuple must use the same key type as the dispatch Key */
#ifdef PPB_FAIL_PRESCAN_KEY_TYPE_MISMATCH
constexpr auto bad = ppb::reader<SmokeSchema>(smoke_wire, sizeof(smoke_wire))
                         .prescan({}, ppb::on<1L>([](const ppb_field &) -> ppb_error { return PPB_OK; }));
#endif

// auto_schema: a non-field leaf (top-level) is rejected.

/* expect-error: auto_schema arguments must be field_generic_base */
#ifdef PPB_FAIL_AUTO_SCHEMA_NOT_A_FIELD
static_assert(sizeof(ppb::auto_schema<ppb::varint<1>, int>) > 0);
#endif

// auto_schema: a non-field leaf nested inside a tuple is rejected.

/* expect-error: auto_schema arguments must be field_generic_base */
#ifdef PPB_FAIL_AUTO_SCHEMA_NESTED_NOT_A_FIELD
static_assert(sizeof(ppb::auto_schema<std::tuple<ppb::varint<1>, int>>) > 0);
#endif

// auto_schema: duplicate (key, wire) pair is caught by schema's
// strict-ascending check after sorting.

/* expect-error: schema fields must be listed in strictly ascending order */
#ifdef PPB_FAIL_AUTO_SCHEMA_DUPLICATE
static_assert(sizeof(ppb::auto_schema<ppb::varint<2>, ppb::varint<2>>) > 0);
#endif

// auto_schema: empty input flattens to an empty schema, which is rejected.

/* expect-error: schema must include at least one field */
#ifdef PPB_FAIL_AUTO_SCHEMA_EMPTY
static_assert(sizeof(ppb::auto_schema<>) > 0);
#endif

/* expect-error: schema must include at least one field */
#ifdef PPB_FAIL_AUTO_SCHEMA_EMPTY_TUPLE
static_assert(sizeof(ppb::auto_schema<std::tuple<>>) > 0);
#endif

// auto_schema: heterogeneous Key types are still rejected post-sort.

/* expect-error: schema fields must all have the same Key type */
#ifdef PPB_FAIL_AUTO_SCHEMA_DIFFERENT_KEYS
static_assert(sizeof(ppb::auto_schema<ppb::varint<1>, ppb::varint<1L>>) > 0);
#endif

// unknown<>: only the four real wire types are accepted.

/* expect-error: ppb::unknown wire type must be one of varint, i64, len, or i32 */
#ifdef PPB_FAIL_UNKNOWN_WIRE_ANY
static_assert(sizeof(ppb::unknown<ppb::wire_type::any>) > 0);
#endif

/* expect-error: ppb::unknown wire type must be one of varint, i64, len, or i32 */
#ifdef PPB_FAIL_UNKNOWN_WIRE_BOGUS
static_assert(sizeof(ppb::unknown<static_cast<ppb::wire_type>(3)>) > 0);
#endif

// Manual schema with a catch-all placed *before* a real field violates
// the strict-ascending check (catch-all encoded tag bits are larger
// than any real-field tag).

/* expect-error: schema fields must be listed in strictly ascending order */
#ifdef PPB_FAIL_UNKNOWN_BEFORE_REAL_FIELD
static_assert(sizeof(ppb::schema<ppb::unknown<ppb::wire_type::varint>, ppb::varint<1>>) > 0);
#endif

// Orphaned handler: on<Key> with a key absent from the schema.
//
// The static_assert fires from reader::run_handlers when no field
// in the schema matches (Key, wire).

enum class HungryKey : uint64_t
{
    a = 1,
};

/* expect-error: on<Key> or on_unknown<> handler does not match any schema field */
#ifdef PPB_FAIL_RUN_HANDLERS_ORPHANED_ON
static void
trigger_run_handlers_orphaned_on()
{
    ppb::reader<ppb::schema<ppb::varint<HungryKey::a>>>().prescan({},
        ppb::on<HungryKey(2)>([](const ppb_field &) -> ppb_error { return PPB_OK; }));
}
#endif

// Orphaned on<Key> handler: matching Key in schema but wrong wire type.
//
// Key=1 appears only as varint; the handler is pinned to i64, so
// no schema field matches.

/* expect-error: on<Key> or on_unknown<> handler does not match any schema field */
#ifdef PPB_FAIL_RUN_HANDLERS_WRONG_WIRE
static void
trigger_run_handlers_wrong_wire()
{
    ppb::reader<SmokeSchema>().prescan({},
        ppb::on<1, ppb::wire_type::i64>([](const ppb_field &) -> ppb_error { return PPB_OK; }));
}
#endif

// Orphaned on_unknown handler: no ppb::unknown<wire> entry in schema.
//
// The handler targets varint, but the schema has no catch-all for it.

/* expect-error: on<Key> or on_unknown<> handler does not match any schema field */
#ifdef PPB_FAIL_RUN_HANDLERS_ORPHANED_ON_UNKNOWN
static void
trigger_run_handlers_orphaned_unknown()
{
    ppb::reader<SmokeSchema>().prescan({},
        ppb::on_unknown([](const ppb_field &) -> ppb_error { return PPB_OK; }));
}
#endif

// on_unknown pinned to specific wire that is registered in the
// schema as a different wire.
//
// Schema has ppb::unknown<varint>, handler is on_unknown<i64> -> no match.

/* expect-error: on<Key> or on_unknown<> handler does not match any schema field */
#ifdef PPB_FAIL_RUN_HANDLERS_ON_UNKNOWN_WRONG_WIRE
using WrongWireS = ppb::schema<ppb::varint<1>, ppb::unknown<ppb::wire_type::varint>>;
static void
trigger_run_handlers_unknown_wrong_wire()
{
    ppb::reader<WrongWireS>().prescan({},
        ppb::on_unknown<ppb::wire_type::i64>([](const ppb_field &) -> ppb_error { return PPB_OK; }));
}
#endif

// on_unknown: ambiguous handlers (multiple match the same wire and
// both are invocable with the dispatch arg).

/* expect-error: multiple unknown_handlers match the wire type and accept the argument type; ambiguous */
#ifdef PPB_FAIL_ON_UNKNOWN_AMBIGUOUS
constexpr auto bad = ppb::detail::find_unknown_handler<ppb::wire_type::varint, const ppb_field &,
    decltype(ppb::on_unknown([](const ppb_field &) -> ppb_error { return PPB_OK; })),
    decltype(ppb::on_unknown([](const ppb_field &) -> ppb_error { return PPB_OK; }))>();
#endif

// on_unknown: handlers that match the wire but none accepts the
// dispatch arg.  Both lambdas take incompatible argument types.

/* expect-error: unknown_handlers match the wire type, but none is invocable with the argument type */
#ifdef PPB_FAIL_ON_UNKNOWN_NONE_INVOCABLE
constexpr auto bad = ppb::detail::find_unknown_handler<ppb::wire_type::varint, const ppb_field &,
    decltype(ppb::on_unknown([](int) -> ppb_error { return PPB_OK; })),
    decltype(ppb::on_unknown([](const char *) -> ppb_error { return PPB_OK; }))>();
#endif
