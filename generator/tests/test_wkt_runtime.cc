/*
 * Runtime test: end-to-end decode of a user message that embeds all six
 * supported well-known types.
 *
 * wkt_user3.proto's `All` message has one field per supported WKT
 * (Timestamp, Duration, BoolValue, Int32Value, StringValue, FieldMask, Any,
 * Empty).  Each is a length-delimited submessage whose schema lives in the
 * fused include/ppb/wkt.ppb.hpp, pulled in transitively by the generated
 * golden header.  The test hand-encodes one `All` message, drives a
 * ppb::reader over All::schema with on_submessage handlers for each WKT, and
 * asserts the inner field values.
 *
 * The test includes the committed golden header directly so it catches
 * regressions in the generator (and in the fused WKT schemas) before the
 * golden files are refreshed.
 */
#include "golden/wkt_user3.ppb.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{

int g_failures = 0;

#define CHECK(cond)                                                     \
    do                                                                  \
    {                                                                   \
        if (!(cond))                                                    \
        {                                                               \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                               \
        }                                                               \
    } while (0)

namespace All = ppb_gen::wktdemo::All;
namespace Timestamp = ppb_gen::google::protobuf::Timestamp;
namespace Duration = ppb_gen::google::protobuf::Duration;
namespace BoolValue = ppb_gen::google::protobuf::BoolValue;
namespace Int32Value = ppb_gen::google::protobuf::Int32Value;
namespace StringValue = ppb_gen::google::protobuf::StringValue;
namespace FieldMask = ppb_gen::google::protobuf::FieldMask;
namespace Any = ppb_gen::google::protobuf::Any;
/*
 * Lean mode (the generator default) emits merge_schema for WKT submessage
 * references: it uses last_write_wins rather than proto3_zero_default for
 * inner scalar fields.  on_submessage<K, S> requires S to match the schema
 * stored in All::schema exactly, so we must use merge_schema here.
 * (Empty and FieldMask have identical schema/merge_schema, but we use
 * merge_schema throughout for consistency.)
 */
using Empty_schema = ppb_gen::google::protobuf::Empty::merge_schema;

/*
 * Hand-encoded `All`:
 *   at    = Timestamp{ seconds=5, nanos=7 }
 *   took  = Duration{ seconds=9 }
 *   flag  = BoolValue{ value=true }
 *   count = Int32Value{ value=42 }
 *   label = StringValue{ value="hi" }
 *   mask  = FieldMask{ paths=["a", "b.c"] }
 *   blob  = Any{ type_url="type.googleapis.com/X", value={0xde, 0xad} }
 *   nothing = Empty{}  (present, no fields)
 *
 * Each outer field is tag (field<<3 | 2) + length + inner submessage bytes.
 * Verified byte-for-byte against the protobuf reference parser.
 */
const uint8_t all_wire[] = {
    /* field 1 (at): Timestamp{seconds=5, nanos=7} */
    0x0a,
    0x04,
    0x08,
    0x05,
    0x10,
    0x07,
    /* field 2 (took): Duration{seconds=9} */
    0x12,
    0x02,
    0x08,
    0x09,
    /* field 3 (flag): BoolValue{value=true} */
    0x1a,
    0x02,
    0x08,
    0x01,
    /* field 4 (count): Int32Value{value=42} */
    0x22,
    0x02,
    0x08,
    0x2a,
    /* field 5 (label): StringValue{value="hi"} */
    0x2a,
    0x04,
    0x0a,
    0x02,
    0x68,
    0x69,
    /* field 6 (mask): FieldMask{paths=["a", "b.c"]} */
    0x32,
    0x08,
    0x0a,
    0x01,
    0x61,
    0x0a,
    0x03,
    0x62,
    0x2e,
    0x63,
    /* field 7 (blob): Any{type_url="type.googleapis.com/X", value={0xde,0xad}} */
    0x3a,
    0x1b,
    0x0a,
    0x15,
    0x74,
    0x79,
    0x70,
    0x65,
    0x2e,
    0x67,
    0x6f,
    0x6f,
    0x67,
    0x6c,
    0x65,
    0x61,
    0x70,
    0x69,
    0x73,
    0x2e,
    0x63,
    0x6f,
    0x6d,
    0x2f,
    0x58,
    0x12,
    0x02,
    0xde,
    0xad,
    /* field 8 (nothing): Empty{} */
    0x42,
    0x00,
};

void
test_decode_all_wkts()
{
    std::printf("test_decode_all_wkts\n");

    std::span<const std::byte> bytes(reinterpret_cast<const std::byte *>(all_wire), sizeof(all_wire));

    /* Decoded destinations, one per WKT inner field. */
    int64_t ts_seconds = 0;
    int32_t ts_nanos = 0;
    int64_t dur_seconds = 0;
    bool flag_value = false;
    int32_t count_value = 0;
    std::string label;
    std::vector<std::string> mask_paths;
    std::string any_type_url;
    std::vector<uint8_t> any_value;
    bool nothing_seen = false;

    ppb::reader<All::schema> r(bytes);

    /*
     * All::schema embeds eight submessages, so the outer parse must grant a
     * recursion budget of at least one level for the on_submessage handlers.
     */
    ppb_error err = r.parse(ppb::limit::max_depth(1),
        ppb::on_submessage<All::F::at, Timestamp::merge_schema>(
            ppb::on<Timestamp::F::seconds>(
                [&](int64_t v) -> ppb_error
                {
                    ts_seconds = v;
                    return PPB_OK;
                }),
            ppb::on<Timestamp::F::nanos>(
                [&](int32_t v) -> ppb_error
                {
                    ts_nanos = v;
                    return PPB_OK;
                })),
        ppb::on_submessage<All::F::took, Duration::merge_schema>(
            ppb::on<Duration::F::seconds>(
                [&](int64_t v) -> ppb_error
                {
                    dur_seconds = v;
                    return PPB_OK;
                })),
        ppb::on_submessage<All::F::flag, BoolValue::merge_schema>(
            ppb::on<BoolValue::F::value>(
                [&](bool v) -> ppb_error
                {
                    flag_value = v;
                    return PPB_OK;
                })),
        ppb::on_submessage<All::F::count, Int32Value::merge_schema>(
            ppb::on<Int32Value::F::value>(
                [&](int32_t v) -> ppb_error
                {
                    count_value = v;
                    return PPB_OK;
                })),
        ppb::on_submessage<All::F::label, StringValue::merge_schema>(
            ppb::on<StringValue::F::value>(
                [&](std::string_view sv) -> ppb_error
                {
                    label.assign(sv);
                    return PPB_OK;
                })),
        ppb::on_submessage<All::F::mask, FieldMask::merge_schema>(
            ppb::on<FieldMask::F::paths>(
                [&](std::string_view sv) -> ppb_error
                {
                    mask_paths.emplace_back(sv);
                    return PPB_OK;
                })),
        ppb::on_submessage<All::F::blob, Any::merge_schema>(
            ppb::on<Any::F::type_url>(
                [&](std::string_view sv) -> ppb_error
                {
                    any_type_url.assign(sv);
                    return PPB_OK;
                }),
            ppb::on<Any::F::value>(
                [&](std::span<const std::byte> v) -> ppb_error
                {
                    for (std::byte b : v)
                    {
                        any_value.push_back(static_cast<uint8_t>(b));
                    }

                    return PPB_OK;
                })),
        ppb::on_submessage<All::F::nothing, Empty_schema>(
            [&](const ppb::reader<Empty_schema> &) -> ppb_error
            {
                nothing_seen = true;
                return PPB_OK;
            }));

    CHECK(err == PPB_OK);
    CHECK(r.error() == PPB_OK);

    CHECK(ts_seconds == 5);
    CHECK(ts_nanos == 7);
    CHECK(dur_seconds == 9);
    CHECK(flag_value == true);
    CHECK(count_value == 42);
    CHECK(label == "hi");

    CHECK(mask_paths.size() == 2);
    if (mask_paths.size() == 2)
    {
        CHECK(mask_paths[0] == "a");
        CHECK(mask_paths[1] == "b.c");
    }

    CHECK(any_type_url == "type.googleapis.com/X");
    CHECK(any_value.size() == 2);
    if (any_value.size() == 2)
    {
        CHECK(any_value[0] == 0xde);
        CHECK(any_value[1] == 0xad);
    }

    CHECK(nothing_seen);
}

}  // namespace

int
main()
{
    test_decode_all_wkts();

    std::printf("\n%d failures\n", g_failures);
    return g_failures > 0 ? 1 : 0;
}
