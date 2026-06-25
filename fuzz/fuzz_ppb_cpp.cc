/*
 * LibFuzzer harness for the PPB C++ wrapper (`ppb::reader`).
 *
 * The C fuzzer (fuzz/fuzz_ppb.c) exercises the C core API.  This
 * harness drives the template machinery in <ppb/ppb.hpp> +
 * <ppb/ppb_detail.hpp>.  Sanitizers catch UB; POSTCOND traps on some
 * invariant violations.
 *
 * Each config lives in its own namespace; the shared fixtures (Schema A
 * and the dispatch-counting handler tuple) sit at the top.
 */
#include "ppb/ppb.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
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

/*
 * Shared fixture: a proto3 message with one scalar field of every wire type,
 * plus unknown-field detection.  Used by several configs below.
 */
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

/*
 * Outcome of running one reader operation: the sticky error, the prescan
 * return value, and per-field dispatch counts.  calls[0..4] are Schema A's
 * five known fields; calls[5] is the unknown catch-all.  Shared by the
 * api-sequence and C-vs-C++ differential configs.
 */
struct op_result
{
    ppb_error err;
    ptrdiff_t scanned;
    size_t calls[6];
};

bool
op_result_equal(const op_result &a, const op_result &b)
{
    if (a.err != b.err || a.scanned != b.scanned)
        return false;

    for (size_t i = 0; i < 6; i++)
    {
        if (a.calls[i] != b.calls[i])
            return false;
    }

    return true;
}

/* Handler tuple over Schema A that only counts dispatches into op_result. */
auto
make_counting_tuple(op_result *res)
{
    return std::tuple {
        ppb::on<FA::i>(
            [res](int32_t) -> ppb_error
            {
                res->calls[0]++;
                return PPB_OK;
            }),
        ppb::on<FA::u>(
            [res](uint64_t) -> ppb_error
            {
                res->calls[1]++;
                return PPB_OK;
            }),
        ppb::on<FA::d>(
            [res](double) -> ppb_error
            {
                res->calls[2]++;
                return PPB_OK;
            }),
        ppb::on<FA::f>(
            [res](float) -> ppb_error
            {
                res->calls[3]++;
                return PPB_OK;
            }),
        ppb::on<FA::s>(
            [res](std::string_view) -> ppb_error
            {
                res->calls[4]++;
                return PPB_OK;
            }),
        ppb::on_unknown<>(
            [res](const ppb_field &) -> ppb_error
            {
                res->calls[5]++;
                return PPB_OK;
            }),
    };
}

/*
 * Proto3 scalars (every wire type) + unknown-field detection.  Exercises the
 * range-check dispatch and the proto3 zero-default widening path in
 * reader::run_handler_for_idx / dispatch_tuple.
 */
namespace proto3_scalars
{

void
exercise_proto3_scalars(std::span<const std::byte> input)
{
    size_t calls[5] = { 0, 0, 0, 0, 0 };
    size_t unknown_calls = 0;
    bool saw_zero_unknown = false;

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

    /* If we saw a zero field, we definitely saw an unknown. */
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

}  // namespace proto3_scalars

/*
 * Packed-varint and packed-fixed views + repeated bytes.  Exercises
 * packed_varint_iter (lazy varint decode of malformed payloads) and the
 * le_packed<T> span reinterpretation.
 */
namespace packed_views
{

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
exercise_packed_views(std::span<const std::byte> input)
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
        /* At least one byte per varint. */
        POSTCOND(pv.size() <= r.meta<FB::pv>().total_bytes);
        /* A packed-fixed payload of N elements is N*sizeof(element) bytes. */
        POSTCOND(p4.size() == r.meta<FB::p4>().total_bytes / 4);
        POSTCOND(p8.size() == r.meta<FB::p8>().total_bytes / 8);
        POSTCOND(num_bytes_seen == r.meta<FB::by>().total_bytes);
    }
}

}  // namespace packed_views

/*
 * Three-level submessage nesting; sweep max_depth to trigger the
 * DEPTH_EXCEEDED boundary and submessage handling.
 */
namespace submessage_depth
{

enum class FC : uint32_t
{
    val = 1,   // varint payload
    sub = 2,   // nested message
};

using SchemaLeaf = ppb::schema<ppb::proto3_uint32<FC::val>>;
using SchemaMid = ppb::schema<ppb::proto3_uint32<FC::val>, ppb::message<FC::sub, SchemaLeaf>>;
using SchemaTop = ppb::schema<ppb::proto3_uint32<FC::val>, ppb::message<FC::sub, SchemaMid>>;

void
exercise_submessage_depth(std::span<const std::byte> input)
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

}  // namespace submessage_depth

/*
 * Drive one reader through multiple passes without reset_fields().  Metadata
 * accumulates across prescans and the sticky error never clears; this confirms
 * reuse stays UB-free and monotone.
 */
namespace reader_reuse
{

void
exercise_reader_reuse(std::span<const std::byte> input)
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

}  // namespace reader_reuse

/*
 * Stateful API-sequence program over one reader.  input[0] picks how many
 * opcode bytes follow; the rest is the wire buffer.  Ops interleave prescan /
 * parse / lexn / lex_all / reset_fields, including reuse *without*
 * reset_fields(), and assert the stateful cross-call invariants:
 *
 *   - sticky-error short-circuit: the same error is returned, the input span
 *     does not advance, metadata is untouched, and no handler fires;
 *   - error() is monotone: PPB_OK -> E at most once, then constant;
 *   - input() is always a suffix of the constructing span, and prescan() never
 *     advances it;
 *   - unknown_field() is soft-sticky: once set, never changes;
 *   - reset_fields() restores fresh per-field metadata, preserves
 *     error()/unknown_field(), and an error-free reader behaves exactly like a
 *     freshly-constructed one afterwards (checked differentially);
 *   - readers are value types: a copy executes the same op with identical
 *     observable results (checked differentially).
 */
namespace api_sequence
{

struct meta_snapshot
{
    ppb_field_meta m[5];
};

meta_snapshot
snap_meta(const ppb::reader<SchemaA> &r)
{
    return { { r.meta<FA::i>(), r.meta<FA::u>(), r.meta<FA::d>(), r.meta<FA::f>(), r.meta<FA::s>() } };
}

bool
meta_equal(const meta_snapshot &a, const meta_snapshot &b)
{
    for (size_t i = 0; i < 5; i++)
    {
        if (a.m[i].lost_distinct_u64 != b.m[i].lost_distinct_u64 ||
            a.m[i].num_occurrences != b.m[i].num_occurrences || a.m[i].total_bytes != b.m[i].total_bytes ||
            a.m[i].min_nonzero_bytes != b.m[i].min_nonzero_bytes || a.m[i].max_bytes != b.m[i].max_bytes)
            return false;
    }

    return true;
}

/* Runs prescan (0), parse (1), lexn (2), or lex_all (3) under `bounds`. */
op_result
do_simple_op(ppb::reader<SchemaA> &r, unsigned which, ppb::limit bounds)
{
    op_result res = {};
    auto tup = make_counting_tuple(&res);

    switch (which & 3)
    {
    case 0:
        res.scanned = r.prescan(bounds);
        break;

    case 1:
        (void)r.parse_tuple(std::nullopt, bounds, tup);
        break;

    case 2:
        (void)r.lexn_tuple(bounds, tup);
        break;

    case 3:
        (void)r.lex_all_tuple(bounds, tup);
        break;
    }

    res.err = r.error();
    return res;
}

void
exercise_api_sequence(std::span<const std::byte> input)
{
    if (input.empty())
        return;

    const size_t n_ops = static_cast<uint8_t>(input[0]) & 15;
    if (input.size() < 1 + n_ops)
        return;

    const auto ops = input.subspan(1, n_ops);
    const auto buf = input.subspan(1 + n_ops);

    /* Reference for "reset_fields() restores fresh metadata". */
    const meta_snapshot fresh_meta = snap_meta(ppb::reader<SchemaA>(buf));

    ppb::reader<SchemaA> r(buf);

    for (const std::byte raw : ops)
    {
        const auto op = static_cast<uint8_t>(raw);
        const unsigned arg = op >> 3;

        const ppb_error before_err = r.error();
        const auto before_in = r.input();
        const meta_snapshot before_meta = snap_meta(r);
        const auto before_unknown = r.unknown_field();

        switch (op & 7)
        {
        case 0:
        case 7:
        {
            const ppb::limit bounds = (op & 7) == 7 ? ppb::limit::soft(arg) : ppb::limit {};
            const op_result res = do_simple_op(r, /*which=*/0, bounds);

            /* prescan never advances the input span. */
            POSTCOND(r.input().data() == before_in.data() && r.input().size() == before_in.size());

            if (before_err != PPB_OK)
                POSTCOND(res.scanned == ptrdiff_t(before_err));

            break;
        }

        case 1:
        case 2:
        case 3:
        {
            const op_result res = do_simple_op(r, op & 3, ppb::limit {});

            if (before_err != PPB_OK)
            {
                /* Sticky short-circuit: nothing observable happens. */
                POSTCOND(res.err == before_err);
                POSTCOND(r.input().data() == before_in.data() && r.input().size() == before_in.size());
                POSTCOND(meta_equal(snap_meta(r), before_meta));

                for (size_t calls : res.calls)
                {
                    POSTCOND(calls == 0);
                }
            }

            break;
        }

        case 4:
        {
            r.reset_fields();

            POSTCOND(r.error() == before_err);
            POSTCOND(meta_equal(snap_meta(r), fresh_meta));
            POSTCOND(r.input().data() == before_in.data() && r.input().size() == before_in.size());

            if (before_err == PPB_OK)
            {
                /*
                 * An error-free reader after reset_fields() must be
                 * indistinguishable from a fresh reader over input() (modulo
                 * unknown_field(), which reset_fields() preserves).
                 */
                ppb::reader<SchemaA> from_reset = r;
                ppb::reader<SchemaA> fresh(r.input());
                const op_result a = do_simple_op(from_reset, arg & 3, ppb::limit {});
                const op_result b = do_simple_op(fresh, arg & 3, ppb::limit {});

                POSTCOND(op_result_equal(a, b));
                POSTCOND(from_reset.error() == fresh.error());
                POSTCOND(from_reset.input().data() == fresh.input().data() &&
                    from_reset.input().size() == fresh.input().size());
                POSTCOND(meta_equal(snap_meta(from_reset), snap_meta(fresh)));
            }

            break;
        }

        case 5:
        {
            /* Value-type check: a copy executes the same op identically. */
            ppb::reader<SchemaA> copy = r;
            const op_result a = do_simple_op(r, arg & 3, ppb::limit {});
            const op_result b = do_simple_op(copy, arg & 3, ppb::limit {});

            POSTCOND(op_result_equal(a, b));
            POSTCOND(r.error() == copy.error());
            POSTCOND(r.input().data() == copy.input().data() && r.input().size() == copy.input().size());
            POSTCOND(meta_equal(snap_meta(r), snap_meta(copy)));
            break;
        }

        case 6:
        {
            const op_result res = do_simple_op(r, /*which=*/1, ppb::limit::hard(arg));

            if (before_err != PPB_OK)
                POSTCOND(res.err == before_err);

            break;
        }
        }

        /* Cross-call contract, after every op. */
        if (before_err != PPB_OK)
            POSTCOND(r.error() == before_err);

        if (before_unknown.has_value())
            POSTCOND(r.unknown_field() == before_unknown);

        /* input() is always a suffix of the constructing span. */
        POSTCOND(r.input().data() >= buf.data());
        POSTCOND(r.input().data() + r.input().size() == buf.data() + buf.size());
    }
}

}  // namespace api_sequence

/*
 * C-vs-C++ differential over Schema A.  The C-side tag array is built
 * independently with PPB_TAG and matches SchemaA::s_encoded_tags.  The same
 * input goes through the raw C API and a ppb::reader; prescan and a lockstep
 * lexn loop must agree on consumption, error codes, and the full per-field
 * state of the five specific slots (the catch-all slots are not reachable
 * through the wrapper API).
 */
namespace c_vs_cpp_differential
{

constexpr ppb_encoded_tag k_tags[] = {
    PPB_TAG(1, PPB_WIRE_VARINT),
    PPB_TAG(2, PPB_WIRE_VARINT),
    PPB_TAG(3, PPB_WIRE_I64),
    PPB_TAG(4, PPB_WIRE_I32),
    PPB_TAG(5, PPB_WIRE_LEN),
    PPB_TAG(-1, PPB_WIRE_VARINT),
    PPB_TAG(-1, PPB_WIRE_I64),
    PPB_TAG(-1, PPB_WIRE_LEN),
    PPB_TAG(-1, PPB_WIRE_I32),
};
constexpr size_t k_num_tags = sizeof(k_tags) / sizeof(k_tags[0]);

static_assert(
    []
    {
        if (SchemaA::s_encoded_tags.size() != k_num_tags)
            return false;

        for (size_t i = 0; i < k_num_tags; i++)
        {
            if (SchemaA::s_encoded_tags[i].bits != k_tags[i].bits)
                return false;
        }

        return true;
    }(),
    "the C-vs-C++ differential's hand-built tag array must mirror Schema A's");

bool
field_state_equal(const ppb_field &a, const ppb_field &b)
{
    return a.m.lost_distinct_u64 == b.m.lost_distinct_u64 && a.m.num_occurrences == b.m.num_occurrences &&
        a.m.total_bytes == b.m.total_bytes && a.m.min_nonzero_bytes == b.m.min_nonzero_bytes &&
        a.m.max_bytes == b.m.max_bytes && a.v.ptr == b.v.ptr && a.v.u64 == b.v.u64 &&
        a.v.payload.buf == b.v.payload.buf && a.v.payload.size == b.v.payload.size;
}

std::array<ppb_field, 5>
snap_specific_fields(const ppb::reader<SchemaA> &r)
{
    return { r.field<FA::i>(), r.field<FA::u>(), r.field<FA::d>(), r.field<FA::f>(), r.field<FA::s>() };
}

void
exercise_c_vs_cpp_differential(std::span<const std::byte> input)
{
    POSTCOND(ppb_validate_tags(k_num_tags, k_tags) == PPB_OK);

    /* Prescan differential. */
    {
        ppb_field c_fields[k_num_tags] = {};
        const ppb_buf c_buf = { .buf = input.data(), .size = input.size() };
        const ptrdiff_t c_scanned = ppb_prescan(c_buf, k_num_tags, k_tags, c_fields, SIZE_MAX);

        ppb::reader<SchemaA> r(input);
        const ptrdiff_t cpp_scanned = r.prescan();

        POSTCOND(cpp_scanned == c_scanned);
        POSTCOND(r.error() == (c_scanned < 0 ? ppb_error(c_scanned) : PPB_OK));

        const auto cpp_fields = snap_specific_fields(r);
        for (size_t i = 0; i < cpp_fields.size(); i++)
        {
            POSTCOND(field_state_equal(cpp_fields[i], c_fields[i]));
        }
    }

    /* Lockstep lexn-loop differential. */
    ppb_field c_fields[k_num_tags] = {};
    ppb_buf c_buf = { .buf = input.data(), .size = input.size() };
    ppb::reader<SchemaA> r(input);

    for (size_t iter = 0; r.error() == PPB_OK && !r.input().empty(); iter++)
    {
        /* Every clean batch consumes at least one byte. */
        POSTCOND(iter < input.size());

        ppb_field c_before[k_num_tags];
        for (size_t i = 0; i < k_num_tags; i++)
        {
            c_before[i] = c_fields[i];
        }

        op_result res = {};
        auto tup = make_counting_tuple(&res);
        const ppb_error cpp_err = r.lexn_tuple(ppb::limit {}, tup);

        const ppb_lexn_ret ret = ppb_lexn(&c_buf, k_num_tags, k_tags, c_fields, SIZE_MAX);

        POSTCOND(cpp_err == ret.status);
        POSTCOND(r.error() == ret.status);
        POSTCOND(r.input().size() == c_buf.size);

        const auto cpp_fields = snap_specific_fields(r);
        for (size_t i = 0; i < cpp_fields.size(); i++)
        {
            POSTCOND(field_state_equal(cpp_fields[i], c_fields[i]));
        }

        /* The dispatch-count oracle only applies to clean batches. */
        if (ret.status != PPB_OK)
            continue;

        for (size_t i = 0; i < 5; i++)
        {
            POSTCOND(res.calls[i] == (c_fields[i].v.ptr != c_before[i].v.ptr ? 1u : 0u));
        }

        size_t catchall_moved = 0;
        for (size_t i = 5; i < k_num_tags; i++)
        {
            catchall_moved += c_fields[i].v.ptr != c_before[i].v.ptr ? 1 : 0;
        }

        /* A batch always ends after at most one catch-all field. */
        POSTCOND(catchall_moved <= 1);
        POSTCOND(res.calls[5] == catchall_moved);
    }

    POSTCOND(r.input().size() == c_buf.size);
}

}  // namespace c_vs_cpp_differential

}  // namespace

extern "C" int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    std::span<const std::byte> input = as_bytes(data, size);

    proto3_scalars::exercise_proto3_scalars(input);
    packed_views::exercise_packed_views(input);
    submessage_depth::exercise_submessage_depth(input);
    reader_reuse::exercise_reader_reuse(input);
    api_sequence::exercise_api_sequence(input);
    c_vs_cpp_differential::exercise_c_vs_cpp_differential(input);

    return 0;
}
