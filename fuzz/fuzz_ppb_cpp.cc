/*
 * LibFuzzer harness for the PPB C++ wrapper (`ppb::reader`).
 *
 * The C fuzzer (fuzz/fuzz_ppb.c) exercises the C core API.  This
 * harness drives the template machinery in <ppb/ppb.hpp> +
 * <ppb/ppb_detail.hpp>.  Sanitizers catch UB; POSTCOND traps on some
 * invariant violations.
 */
#include "ppb/ppb.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#define POSTCOND(cond)        \
    do                        \
    {                         \
        if (!(cond))          \
            __builtin_trap(); \
    } while (0)

namespace
{

inline std::span<const std::byte>
as_bytes(const uint8_t *data, size_t size)
{
    return std::span<const std::byte>(reinterpret_cast<const std::byte *>(data), size);
}

// Config A: proto3 scalars (every wire type) + unknown-field detection.
// Exercises the range-check dispatch and the proto3 zero-default
// widening path in reader::run_handler_for_idx / dispatch_tuple.
enum class FA : uint32_t
{
    i = 1,   // varint
    u = 2,   // varint
    d = 3,   // i64
    f = 4,   // i32
    s = 5,   // len
};

using SchemaA = ppb::auto_schema<ppb::proto3_int32<FA::i>, ppb::proto3_uint64<FA::u>, ppb::proto3_f64<FA::d>,
    ppb::proto3_f32<FA::f>, ppb::proto3_utf8string<FA::s>, ppb::detect_unknown_fields<>>;

void
exercise_config_a(std::span<const std::byte> input)
{
    size_t calls[5] = { 0, 0, 0, 0, 0 };
    size_t unknown_calls = 0;
    bool saw_zero_unknown = 0;

    ppb::reader<SchemaA> r(input);
    ppb_error err = r.parse(ppb::on<FA::i>(
                                [&](int32_t) -> ppb_error
                                {
                                    calls[0]++;
                                    return PPB_OK;
                                }),
        ppb::on<FA::u>(
            [&](uint64_t) -> ppb_error
            {
                calls[1]++;
                return PPB_OK;
            }),
        ppb::on<FA::d>(
            [&](double) -> ppb_error
            {
                calls[2]++;
                return PPB_OK;
            }),
        ppb::on<FA::f>(
            [&](float) -> ppb_error
            {
                calls[3]++;
                return PPB_OK;
            }),
        ppb::on<FA::s>(
            [&](std::string_view) -> ppb_error
            {
                calls[4]++;
                return PPB_OK;
            }),
        ppb::on_unknown<>(
            [&](const ppb_field &field) -> ppb_error
            {
                ppb_error err = PPB_OK;
                ppb_buf buf = {
                    .buf = field.v.ptr,
                    .size = size_t(
                        input.data() + input.size() - reinterpret_cast<const std::byte *>(field.v.ptr)),
                };

                // need full decode to handle non-canonical encodings
                uint64_t tag = ppb_decode_varint(&buf, &err);
                saw_zero_unknown |= tag == 0;

                unknown_calls++;
                return err;
            }));

    if (err != PPB_OK)
        return;

    /*
     * proto3_zero_default / last_write_wins dispatch rules:
     *
     *   Fast path (no field forced lexn): handler fires at most once.
     *     Absent field (num_occ == 0): 0 or 1 calls (zero-default may fire).
     *     Present field (num_occ >= 1): exactly 1 call (last-write value).
     *
     *   Lexn path (some field forced a lexn pass, e.g., unknown fields with
     *   repeated semantics and > 1 occurrence): every occurrence of every
     *   field dispatches, so calls == num_occurrences.
     */
    auto check_lww = [](size_t calls_count, size_t num_occ)
    {
        if (num_occ == 0)
        {
            POSTCOND(calls_count <= 1);
        }
        else
        {
            POSTCOND(calls_count == num_occ || calls_count == 1);
        }
    };

    check_lww(calls[0], r.meta<FA::i>().num_occurrences);
    check_lww(calls[1], r.meta<FA::u>().num_occurrences);
    check_lww(calls[2], r.meta<FA::d>().num_occurrences);
    check_lww(calls[3], r.meta<FA::f>().num_occurrences);
    check_lww(calls[4], r.meta<FA::s>().num_occurrences);

    /*
     * If we saw a zero field, we definitely saw an unknown
     */
    POSTCOND(!saw_zero_unknown || unknown_calls > 0);

    /*
     * A set unknown_field() implies at least one unknown handler fired.
     *
     * The converse does not hold: a non-canonical zero tag (e.g. 0x80
     * 0x00) routes to the catch-all yet collides with unknown_field()'s
     * 0 "unset" sentinel, so the handler can fire with no reported tag....
     * but, in that case, we'll have `saw_zero_unknown`.
     */
    POSTCOND(!r.unknown_field().has_value() || unknown_calls > 0);
    POSTCOND(unknown_calls == 0 || r.unknown_field().has_value() || saw_zero_unknown);
}

/*
 * Config B: packed-varint and packed-fixed views + repeated bytes.
 * Exercises packed_varint_iter (lazy varint decode of malformed
 * payloads) and the le_packed<T> span reinterpretation.
 */
enum class FB : uint32_t
{
    pv = 1,   // packed varint (int32)
    p4 = 2,   // packed fixed32 -> span<le_packed<uint32_t>>
    p8 = 3,   // packed sfixed64 -> span<le_packed<int64_t>>
    by = 4,   // repeated (unpacked) bytes
};

using SchemaB = ppb::auto_schema<ppb::packed_int32<FB::pv>, ppb::packed_fixed32<FB::p4>,
    ppb::packed_sfixed64<FB::p8>, ppb::unpacked_bytes<FB::by>>;

void
exercise_config_b(std::span<const std::byte> input)
{
    std::vector<int32_t> pv;
    std::vector<uint32_t> p4;
    std::vector<int64_t> p8;
    size_t num_bytes_seen = 0;
    uint64_t byte_sink = 0;

    ppb::reader<SchemaB> r(input);
    (void)r.parse(ppb::push_back<FB::pv>(&pv), ppb::push_back<FB::p4>(&p4), ppb::push_back<FB::p8>(&p8),
        ppb::on<FB::by>(
            [&](std::span<const std::byte> b) -> ppb_error
            {
                num_bytes_seen += b.size();

                /* Touch every byte so misaligned/empty payloads are decoded. */
                for (std::byte x : b)
                {
                    byte_sink += static_cast<uint64_t>(x);
                }

                return PPB_OK;
            }));

    __asm__ volatile(" # force " ::"m"(pv), "m"(p4), "m"(p8), "m"(byte_sink) : "memory");
    if (r.error() == PPB_OK)
    {
    /* at least one byte per varint */
        POSTCOND(pv.size() <= r.meta<FB::pv>().total_bytes);
        /* A packed-fixed payload of N elements is N*sizeof(element) bytes. */
        POSTCOND(p4.size() == r.meta<FB::p4>().total_bytes / 4);
        POSTCOND(p8.size() == r.meta<FB::p8>().total_bytes / 8);
        POSTCOND(num_bytes_seen == r.meta<FB::by>().total_bytes);
    }
}

/*
 * Config C: three-level submessage nesting; sweep max_depth to
 * trigger the DEPTH_EXCEEDED boundary and submessage handling.
 */
enum class FC : uint32_t
{
    val = 1,   // varint payload
    sub = 2,   // nested message
};

using SchemaLeaf = ppb::schema<ppb::proto3_uint32<FC::val>>;
using SchemaMid = ppb::schema<ppb::proto3_uint32<FC::val>, ppb::message<FC::sub, SchemaLeaf>>;
using SchemaTop = ppb::schema<ppb::proto3_uint32<FC::val>, ppb::message<FC::sub, SchemaMid>>;

void
exercise_config_c(std::span<const std::byte> input)
{
    uint64_t prev_sum = 0;

    for (uint32_t depth = 0; depth <= 3; depth++)
    {
        uint64_t sum = 0;

        ppb::reader<SchemaTop> r(input);
        (void)r.parse(ppb::limit::max_depth(depth),
            ppb::on<FC::val>(
                [&](uint64_t v) -> ppb_error
                {
                    sum += v;
                    return PPB_OK;
                }),
            ppb::on_submessage<FC::sub, SchemaMid>(ppb::on<FC::val>(
                                                       [&](uint64_t v) -> ppb_error
                                                       {
                                                           sum += v;
                                                           return PPB_OK;
                                                       }),
                ppb::on_submessage<FC::sub, SchemaLeaf>(ppb::on<FC::val>(
                    [&](uint64_t v) -> ppb_error
                    {
                        sum += v;
                        return PPB_OK;
                    }))));

        POSTCOND(sum >= prev_sum);
        prev_sum = sum;
    }
}

/*
 * Config D: drive one reader through multiple passes without
 * reset_fields().  Metadata accumulates across prescans and the sticky
 * error never clears; this confirms reuse stays UB-free and monotone.
 */
void
exercise_config_d(std::span<const std::byte> input)
{
    ppb::reader<SchemaA> r(input);

    ptrdiff_t first = r.prescan();
    uint64_t occ_after_first = r.meta<FA::i>().num_occurrences;

    /* A second prescan without reset_fields() accumulates metadata. */
    ptrdiff_t second = r.prescan();
    if (first >= 0 && second >= 0)
    {
        POSTCOND(r.meta<FA::i>().num_occurrences >= occ_after_first);
    }

    /* The sticky error is monotone across a follow-up parse. */
    ppb_error before = r.error();
    (void)r.parse(ppb::on<FA::u>([](uint64_t) -> ppb_error { return PPB_OK; }));

    if (before != PPB_OK)
    {
        POSTCOND(r.error() == before);
    }

    /* reset_fields() zeros per-field state but must NOT clear error(). */
    ppb_error err_before_reset = r.error();
    r.reset_fields();
    POSTCOND(r.error() == err_before_reset);
}

}  // namespace

extern "C" int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    std::span<const std::byte> input = as_bytes(data, size);

    exercise_config_a(input);
    exercise_config_b(input);
    exercise_config_c(input);
    exercise_config_d(input);

    return 0;
}
