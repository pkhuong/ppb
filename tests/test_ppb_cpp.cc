#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <ppb/ppb.hpp>
#include <string>
#include <string_view>
#include <vector>

static int g_fail_count = 0;
static int g_check_count = 0;

static inline void
check(bool passes, const char *file, int line, const char *cond)
{
    g_check_count++;
    if (passes)
        return;

    std::fprintf(stderr, "  FAIL %s:%d: %s\n", file, line, cond);
    g_fail_count++;
    return;
}

#define CHECK(cond) check(!!(cond), __FILE__, __LINE__, #cond)

/*
 * Smoke tests for ppb::reader.
 *
 * Wire format (same as four_field_wire in test_ppb.c):
 *   field 1 varint 150, field 2 i64 1, field 3 LEN "hello", field 4 i32 42.
 */
using TestSchema = ppb::schema<ppb::varint<1>, ppb::i64<2>, ppb::len<3>, ppb::i32<4>>;

static const uint8_t four_field_wire[] = {
    0x08, 0x96, 0x01,                                     /* field 1 varint 150 */
    0x11, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* field 2 i64 1 */
    0x1a, 0x05, 0x68, 0x65, 0x6c, 0x6c, 0x6f,             /* field 3 LEN "hello" */
    0x25, 0x2a, 0x00, 0x00, 0x00,                         /* field 4 i32 42 */
};

static void
test_reader_default_construct()
{
    ppb::reader<TestSchema> r;

    CHECK(r.error() == PPB_OK);
    CHECK(r.prescan() == 0);
}

static void
test_reader_span_construct()
{
    ppb::reader<TestSchema> r(
        std::span(reinterpret_cast<const std::byte *>(four_field_wire), sizeof(four_field_wire)));

    CHECK(r.error() == PPB_OK);
}

static void
test_reader_ptr_construct()
{
    ppb::reader<TestSchema> r(four_field_wire, sizeof(four_field_wire));

    CHECK(r.error() == PPB_OK);
}

static void
test_reader_ptr_construct_parse()
{
    ppb::reader<TestSchema> r(four_field_wire, sizeof(four_field_wire));

    CHECK(r.prescan() == ptrdiff_t(sizeof(four_field_wire)));
    CHECK(r.error() == PPB_OK);
}

static void
test_reader_prescan_ok()
{
    ppb::reader<TestSchema> r(four_field_wire, sizeof(four_field_wire));

    CHECK(r.prescan() > 0);
    CHECK(r.error() == PPB_OK);
}

static void
test_reader_prescan_empty()
{
    ppb::reader<TestSchema> r(std::span<const std::byte> {});

    CHECK(r.prescan() == 0);
}

static void
test_reader_prescan_with_max_fields()
{
    ppb::reader<TestSchema> r(four_field_wire, sizeof(four_field_wire));

    CHECK(r.prescan(ppb::limit::max_fields(1)) == 3);
}

static void
test_reader_prescan_with_hard_limit()
{
    ppb::reader<TestSchema> r(four_field_wire, sizeof(four_field_wire));

    /* Limit at exactly 3 bytes (end of field 1): succeeds. */
    CHECK(r.prescan(ppb::limit::hard(3)) == 3);
}

static void
test_reader_prescan_hard_limit_exceeded()
{
    ppb::reader<TestSchema> r(four_field_wire, sizeof(four_field_wire));

    /* Limit mid-field-2: LIMIT_EXCEEDED. */
    CHECK(r.prescan(ppb::limit::hard(5)) == PPB_ERROR_LIMIT_EXCEEDED);
    CHECK(r.error() == PPB_ERROR_LIMIT_EXCEEDED);
}

static void
test_reader_prescan_with_soft_limit()
{
    ppb::reader<TestSchema> r(four_field_wire, sizeof(four_field_wire));

    /* Soft limit mid-field-2: consumes through end of field 2, no error. */
    CHECK(r.prescan(ppb::limit::soft(5)) == 12);
}

static void
test_reader_prescan_twice()
{
    ppb::reader<TestSchema> r(four_field_wire, sizeof(four_field_wire));

    CHECK(r.prescan() == sizeof(four_field_wire));
    /* Second prescan re-prescans from scratch. */
    CHECK(r.prescan() == sizeof(four_field_wire));
}

static void
test_reader_prescan_sticky_error()
{
    ppb::reader<TestSchema> r(four_field_wire, sizeof(four_field_wire));

    /*
     * Three-step proof that the error guard short-circuits:
     *   1. Tight limit that ends at a field boundary succeeds.
     *   2. Limit that lands mid-field fails with LIMIT_EXCEEDED.
     *   3. Repeat step 1's limitL must also fail (would succeed
     *      on a fresh reader), proving the guard short-circuits.
     */
    CHECK(r.prescan(ppb::limit::hard(3)) == 3);
    CHECK(r.prescan(ppb::limit::hard(5)) == PPB_ERROR_LIMIT_EXCEEDED);
    CHECK(r.prescan(ppb::limit::hard(3)) == PPB_ERROR_LIMIT_EXCEEDED);
}

// Negative prescan tests

using OneFieldSchema = ppb::schema<ppb::varint<1>>;

// Copied from test_prescan_zero_tag / test_lexn_zero_tag in test_ppb.c.
static const uint8_t zero_tag_wire[] = { 0x00 };

static void
test_reader_prescan_zero_tag()
{
    ppb::reader<OneFieldSchema> r(zero_tag_wire, sizeof(zero_tag_wire));

    CHECK(r.prescan() == PPB_ERROR_CORRUPT_TAG);
    CHECK(r.error() == PPB_ERROR_CORRUPT_TAG);
}

// Copied from test_lexn_corrupt_tag in test_ppb.c
static const uint8_t corrupt_tag_wire[] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,  // 8 continuation bytes, then nothing; invalid varint.
};

static void
test_reader_prescan_corrupt_tag()
{
    ppb::reader<OneFieldSchema> r(corrupt_tag_wire, sizeof(corrupt_tag_wire));

    CHECK(r.prescan() == PPB_ERROR_CORRUPT_TAG);
    CHECK(r.error() == PPB_ERROR_CORRUPT_TAG);
}

// Copied from test_prescan_truncated_tag / test_lexn_truncated_tag in test_ppb.c.
static const uint8_t truncated_tag_wire[] = { 0x80 };

static void
test_reader_prescan_truncated_tag()
{
    ppb::reader<OneFieldSchema> r(truncated_tag_wire, sizeof(truncated_tag_wire));

    CHECK(r.prescan() == PPB_ERROR_TRUNCATED_DATA);
    CHECK(r.error() == PPB_ERROR_TRUNCATED_DATA);
}

// Copied from test_lexn_error in test_ppb.c: valid tag, no value follows.
static const uint8_t truncated_value_wire[] = { 0x08 };

static void
test_reader_prescan_truncated_value()
{
    ppb::reader<OneFieldSchema> r(truncated_value_wire, sizeof(truncated_value_wire));

    CHECK(r.prescan() == PPB_ERROR_TRUNCATED_DATA);
    CHECK(r.error() == PPB_ERROR_TRUNCATED_DATA);
}

// Copied from test_prescan_v_ptr_null_on_error in test_ppb.c:
// valid field 1 varint 1, then field 1 varint with a truncated value.
static const uint8_t valid_then_truncated_wire[] = {
    0x08, 0x01,  /* field 1 varint 1 */
    0x08, 0x80,  /* field 1 varint (truncated, no terminating byte) */
};

static void
test_reader_prescan_valid_then_truncated()
{
    ppb::reader<OneFieldSchema> r(valid_then_truncated_wire, sizeof(valid_then_truncated_wire));

    CHECK(r.prescan() == PPB_ERROR_TRUNCATED_DATA);
    CHECK(r.error() == PPB_ERROR_TRUNCATED_DATA);
}

// Copied from test_lexn_zero_tag_after_valid in test_ppb.c:
// valid field 1 varint 1, then a zero tag byte.
static const uint8_t valid_then_zero_tag_wire[] = {
    0x08, 0x01,  /* field 1 varint 1 */
    0x00,        /* zero tag */
};

static void
test_reader_prescan_valid_then_zero_tag()
{
    ppb::reader<OneFieldSchema> r(valid_then_zero_tag_wire, sizeof(valid_then_zero_tag_wire));

    CHECK(r.prescan() == PPB_ERROR_CORRUPT_TAG);
    CHECK(r.error() == PPB_ERROR_CORRUPT_TAG);
}

// Metadata access tests use enum-keyed schemas, for realism.

// Copied from test_prescan_repeated_fields in test_ppb.c.
static const uint8_t repeated_varint_wire[] = {
    0x08, 0x01,        /* field 1 varint 1  (1-byte value) */
    0x08, 0x96, 0x01,  /* field 1 varint 150 (2-byte value) */
    0x08, 0x01,        /* field 1 varint 1  (1-byte value) */
};

enum class VarintKey : int
{
    my_varint = 1
};
using VarintSchema = ppb::schema<ppb::varint<VarintKey::my_varint>>;

static void
test_reader_meta_repeated_varint()
{
    ppb::reader<VarintSchema> r(repeated_varint_wire, sizeof(repeated_varint_wire));
    CHECK(r.prescan() >= 0);

    ppb_field_meta m = r.meta<VarintKey::my_varint>();
    CHECK(m.num_occurrences == 3);
    CHECK(m.total_bytes == 4);
    CHECK(m.min_nonzero_bytes == 1);
    CHECK(m.max_bytes == 2);
    CHECK(m.lost_distinct_u64 == 1);
}

// Copied from test_prescan_repeated_len in test_ppb.c.
static const uint8_t repeated_len_wire[] = {
    0x0a, 0x00,                   /* field 1 len 0 */
    0x0a, 0x03, 0x00, 0x01, 0x02, /* field 1 len 3 */
    0x0a, 0x02, 0x00, 0x01,       /* field 1 len 2 */
    0x0a, 0x00,                   /* field 1 len 0 */
};

enum class LenKey : int
{
    my_len = 1
};
using LenSchema = ppb::schema<ppb::len<LenKey::my_len>>;

static void
test_reader_meta_repeated_len()
{
    ppb::reader<LenSchema> r(repeated_len_wire, sizeof(repeated_len_wire));
    CHECK(r.prescan() >= 0);

    ppb_field_meta m = r.meta<LenKey::my_len>();
    CHECK(m.num_occurrences == 4);
    CHECK(m.total_bytes == 5);
    CHECK(m.min_nonzero_bytes == 2);
    CHECK(m.max_bytes == 3);
    CHECK(m.lost_distinct_u64 == 0);
}

// Single-occurrence metadata on the four-field wire.
enum class FourKey : int
{
    x = 1,
    y = 2,
    z = 3,
    w = 4
};
using FourSchema =
    ppb::schema<ppb::varint<FourKey::x>, ppb::i64<FourKey::y>, ppb::len<FourKey::z>, ppb::i32<FourKey::w>>;

static void
test_reader_meta_four_field()
{
    ppb::reader<FourSchema> r(four_field_wire, sizeof(four_field_wire));
    CHECK(r.prescan() >= 0);

    ppb_field_meta m1 = r.meta<FourKey::x>();
    CHECK(m1.num_occurrences == 1);
    CHECK(m1.total_bytes == 2);

    ppb_field_meta m2 = r.meta<FourKey::y>();
    CHECK(m2.num_occurrences == 1);
    CHECK(m2.total_bytes == 8);

    ppb_field_meta m3 = r.meta<FourKey::z>();
    CHECK(m3.num_occurrences == 1);
    CHECK(m3.total_bytes == 5);
    CHECK(m3.min_nonzero_bytes == 5);
    CHECK(m3.max_bytes == 5);

    ppb_field_meta m4 = r.meta<FourKey::w>();
    CHECK(m4.num_occurrences == 1);
    CHECK(m4.total_bytes == 4);
}

// Metadata merging: same key, different wire types in the schema.
// The meta() accessor merges consecutive schema entries sharing a field number.
//
// Copied from four_field_wire in test_ppb.c (field 1 varint 150, field 2 i64 1).
static const uint8_t varint_then_len_wire[] = {
    0x08, 0x96, 0x01,                                    /* field 1 varint 150 */
    0x0a, 0x05, 0x68, 0x65, 0x6c, 0x6c, 0x6f,            /* field 1 LEN "hello" */
};

enum class MergeKey : int
{
    x = 1
};
using MergeSchema = ppb::schema<ppb::varint<MergeKey::x>, ppb::len<MergeKey::x>>;

static void
test_reader_meta_merged()
{
    ppb::reader<MergeSchema> r(varint_then_len_wire, sizeof(varint_then_len_wire));
    CHECK(r.prescan() >= 0);

    ppb_field_meta m = r.meta<MergeKey::x>();
    CHECK(m.num_occurrences == 2);
    CHECK(m.total_bytes == 7);
    CHECK(m.lost_distinct_u64 == 1);
    CHECK(m.min_nonzero_bytes == 2);
    CHECK(m.max_bytes == 5);
}

// Specifying a wire type avoids merging: only that wire type's metadata.
static void
test_reader_meta_no_merge()
{
    ppb::reader<MergeSchema> r(varint_then_len_wire, sizeof(varint_then_len_wire));
    CHECK(r.prescan() >= 0);

    ppb_field_meta mv = r.meta<MergeKey::x, ppb::wire_type::varint>();
    CHECK(mv.num_occurrences == 1);
    CHECK(mv.total_bytes == 2);
    CHECK(mv.lost_distinct_u64 == 0);
    CHECK(mv.min_nonzero_bytes == 2);
    CHECK(mv.max_bytes == 2);

    ppb_field_meta ml = r.meta<MergeKey::x, ppb::wire_type::len>();
    CHECK(ml.num_occurrences == 1);
    CHECK(ml.total_bytes == 5);
    CHECK(ml.lost_distinct_u64 == 0);
    CHECK(ml.min_nonzero_bytes == 5);
    CHECK(ml.max_bytes == 5);
}

// When only one wire type appears in the data, the merged result must match
// the result from a schema projected to just that wire type.  Each wire type
// gets its own input bytes, all using field number 1.
static void
test_reader_meta_merged_one_sided()
{
    enum class K : int
    {
        k = 1
    };
    using Merged = ppb::schema<ppb::varint<K::k>, ppb::i64<K::k>, ppb::len<K::k>, ppb::i32<K::k>>;

    // Copied from four_field_wire in test_ppb.c, with tags adjusted to field 1.
    static const uint8_t varint_wire[] = { 0x08, 0x96, 0x01 };  // field 1 varint 150
    static const uint8_t i64_wire[] = { 0x09, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };  // 1 i64 1
    static const uint8_t len_wire[] = { 0x0a, 0x05, 0x68, 0x65, 0x6c, 0x6c, 0x6f };  // 1 LEN "hello"
    static const uint8_t i32_wire[] = { 0x0d, 0x2a, 0x00, 0x00, 0x00 };  // 1 i32 42

    // varint
    {
        using Proj = ppb::schema<ppb::varint<K::k>>;

        ppb::reader<Merged> r(varint_wire, sizeof(varint_wire));
        CHECK(r.prescan() >= 0);

        ppb::reader<Proj> r_proj(varint_wire, sizeof(varint_wire));
        CHECK(r_proj.prescan() >= 0);

        ppb_field_meta m = r.meta<K::k>();
        ppb_field_meta p = r_proj.meta<K::k>();
        CHECK(m.num_occurrences == p.num_occurrences);
        CHECK(m.total_bytes == p.total_bytes);
        CHECK(m.lost_distinct_u64 == p.lost_distinct_u64);
        CHECK(m.min_nonzero_bytes == p.min_nonzero_bytes);
        CHECK(m.max_bytes == p.max_bytes);
    }

    // i64
    {
        using Proj = ppb::schema<ppb::i64<K::k>>;

        ppb::reader<Merged> r(i64_wire, sizeof(i64_wire));
        CHECK(r.prescan() >= 0);

        ppb::reader<Proj> r_proj(i64_wire, sizeof(i64_wire));
        CHECK(r_proj.prescan() >= 0);

        ppb_field_meta m = r.meta<K::k>();
        ppb_field_meta p = r_proj.meta<K::k>();
        CHECK(m.num_occurrences == p.num_occurrences);
        CHECK(m.total_bytes == p.total_bytes);
        CHECK(m.lost_distinct_u64 == p.lost_distinct_u64);
        CHECK(m.min_nonzero_bytes == p.min_nonzero_bytes);
        CHECK(m.max_bytes == p.max_bytes);
    }

    // len
    {
        using Proj = ppb::schema<ppb::len<K::k>>;

        ppb::reader<Merged> r(len_wire, sizeof(len_wire));
        CHECK(r.prescan() >= 0);

        ppb::reader<Proj> r_proj(len_wire, sizeof(len_wire));
        CHECK(r_proj.prescan() >= 0);

        ppb_field_meta m = r.meta<K::k>();
        ppb_field_meta p = r_proj.meta<K::k>();
        CHECK(m.num_occurrences == p.num_occurrences);
        CHECK(m.total_bytes == p.total_bytes);
        CHECK(m.lost_distinct_u64 == p.lost_distinct_u64);
        CHECK(m.min_nonzero_bytes == p.min_nonzero_bytes);
        CHECK(m.max_bytes == p.max_bytes);
    }

    // i32
    {
        using Proj = ppb::schema<ppb::i32<K::k>>;

        ppb::reader<Merged> r(i32_wire, sizeof(i32_wire));
        CHECK(r.prescan() >= 0);

        ppb::reader<Proj> r_proj(i32_wire, sizeof(i32_wire));
        CHECK(r_proj.prescan() >= 0);

        ppb_field_meta m = r.meta<K::k>();
        ppb_field_meta p = r_proj.meta<K::k>();
        CHECK(m.num_occurrences == p.num_occurrences);
        CHECK(m.total_bytes == p.total_bytes);
        CHECK(m.lost_distinct_u64 == p.lost_distinct_u64);
        CHECK(m.min_nonzero_bytes == p.min_nonzero_bytes);
        CHECK(m.max_bytes == p.max_bytes);
    }
}

// Test field() accessor: const, mutable, and cross-check with meta().
static void
test_reader_field_accessor()
{
    static const uint8_t wire[] = { 0x08, 0x96, 0x01 };  // field 1 varint 150
    ppb::reader<VarintSchema> r(wire, sizeof(wire));

    int handler_calls = 0;
    CHECK(r.parse(ppb::on<VarintKey::my_varint>(
              [&handler_calls](const ppb_field &f_h) -> ppb_error
              {
                  handler_calls++;
                  CHECK(f_h.v.u64 == 150);
                  return PPB_OK;
              })) == PPB_OK);
    CHECK(handler_calls == 1);

    // Const accessor: metadata and wire value populated by parse.
    const ppb::reader<VarintSchema> &cr = r;
    const ppb_field &f = cr.field<VarintKey::my_varint>();
    CHECK(f.m.num_occurrences == 1);
    CHECK(f.m.total_bytes == 2);
    CHECK(f.m.min_nonzero_bytes == 2);
    CHECK(f.m.max_bytes == 2);
    CHECK(f.m.lost_distinct_u64 == 0);
    CHECK(f.v.u64 == 150);
    CHECK(f.v.ptr != nullptr);
    CHECK(f.v.ptr == &wire[0]);  // tag byte

    // Cross-check: field().m equals meta() for single-wire case.
    ppb_field_meta fm = cr.meta<VarintKey::my_varint>();
    CHECK(memcmp(&f.m, &fm, sizeof(fm)) == 0);

    // Mutable accessor: zero out the slot and verify.
    ppb_field &mf = r.field<VarintKey::my_varint>();
    mf = ppb_field {};
    CHECK(r.meta<VarintKey::my_varint>().num_occurrences == 0);
}

// Prescan handler dispatch tests

enum class HandlerKey : int
{
    x = 1,
    absent = 42
};

using HandlerSchema =
    ppb::schema<ppb::varint<HandlerKey::x>, ppb::len<HandlerKey::x>, ppb::varint<HandlerKey::absent>>;

// Copied from varint_then_len_wire in test_ppb.c.
static const uint8_t handler_dispatch_wire[] = {
    0x08, 0x96, 0x01,                                    /* field 1 varint 150 */
    0x0a, 0x05, 0x68, 0x65, 0x6c, 0x6c, 0x6f,            /* field 1 LEN "hello" */
};

static void
test_reader_prescan_with_handlers()
{
    int varint_calls = 0;
    int len_calls = 0;
    int absent_calls = 0;

    ppb::reader<HandlerSchema> r(handler_dispatch_wire, sizeof(handler_dispatch_wire));
    CHECK(r.prescan(ppb::limit {},
              ppb::on<HandlerKey::x, ppb::wire_type::varint>(
                  [&varint_calls](const ppb_field &) -> ppb_error
                  {
                      varint_calls++;
                      return PPB_OK;
                  }),
              ppb::on<HandlerKey::x, ppb::wire_type::len>(
                  [&len_calls](const ppb_field &) -> ppb_error
                  {
                      len_calls++;
                      return PPB_OK;
                  }),
              ppb::on<HandlerKey::absent>(
                  [&absent_calls](const ppb_field &) -> ppb_error
                  {
                      absent_calls++;
                      return PPB_OK;
                  })) >= 0);
    CHECK(r.error() == PPB_OK);

    CHECK(varint_calls == 1);
    CHECK(len_calls == 1);
    CHECK(absent_calls == 0);
}

static void
test_reader_prescan_handler_error()
{
    ppb::reader<HandlerSchema> r(handler_dispatch_wire, sizeof(handler_dispatch_wire));

    CHECK(r.prescan(ppb::limit {},
              ppb::on<HandlerKey::x>([](const ppb_field &) -> ppb_error { return PPB_ERROR_CORRUPT_TAG; })) <
        0);
    CHECK(r.error() == PPB_ERROR_CORRUPT_TAG);
}

// lexn handler dispatch tests: each lexn call decodes a batch of
// strictly increasing fields, dispatches handlers, and consumes the
// bytes from the reader's input span.

using LexnSchema = ppb::schema<ppb::varint<1>, ppb::len<2>>;

// Two batches, each {field 1 varint, field 2 LEN}.  The repeated key 1
// at offset 6 forces lexn to end the first batch there.
static const uint8_t lexn_two_batches_wire[] = {
    0x08, 0x01,                 /* batch 1: field 1 varint 1 */
    0x12, 0x02, 0x68, 0x69,     /* batch 1: field 2 LEN "hi" */
    0x08, 0x02,                 /* batch 2: field 1 varint 2 */
    0x12, 0x02, 0x6f, 0x6b,     /* batch 2: field 2 LEN "ok" */
};

static void
test_reader_lexn_decode_dispatch_consume()
{
    int field1_calls = 0;
    int field2_calls = 0;

    ppb::reader<LexnSchema> r(lexn_two_batches_wire, sizeof(lexn_two_batches_wire));

    auto run_once = [&]
    {
        return r.lexn({},
            ppb::on<1>(
                [&](const ppb_field &) -> ppb_error
                {
                    field1_calls++;
                    return PPB_OK;
                }),
            ppb::on<2>(
                [&](const ppb_field &) -> ppb_error
                {
                    field2_calls++;
                    return PPB_OK;
                }));
    };

    // First call decodes batch 1 and fires both handlers.
    CHECK(run_once() == PPB_OK);
    CHECK(field1_calls == 1);
    CHECK(field2_calls == 1);

    // Second call picks up where we left off and decodes batch 2.
    CHECK(run_once() == PPB_OK);
    CHECK(field1_calls == 2);
    CHECK(field2_calls == 2);

    // Third call: input span has been fully consumed, so no handlers fire.
    CHECK(run_once() == PPB_OK);
    CHECK(field1_calls == 2);
    CHECK(field2_calls == 2);
    CHECK(r.error() == PPB_OK);
}

static void
test_reader_lexn_handler_error_runs_full_batch()
{
    int field1_calls = 0;
    int field2_calls = 0;

    ppb::reader<LexnSchema> r(lexn_two_batches_wire, sizeof(lexn_two_batches_wire));

    // Field 1's handler returns an error, but the field 2 handler in
    // the same batch must still fire and the first error must win.
    CHECK(r.lexn({},
              ppb::on<1>(
                  [&](const ppb_field &) -> ppb_error
                  {
                      field1_calls++;
                      return PPB_ERROR_CORRUPT_TAG;
                  }),
              ppb::on<2>(
                  [&](const ppb_field &) -> ppb_error
                  {
                      field2_calls++;
                      return PPB_ERROR_TRUNCATED_DATA;
                  })) == PPB_ERROR_CORRUPT_TAG);

    CHECK(field1_calls == 1);
    CHECK(field2_calls == 1);
    CHECK(r.error() == PPB_ERROR_CORRUPT_TAG);
}

// lexn error (not a handler error, but a decoding error from the C
// lexer itself) bails before invoking any handlers.
static void
test_reader_lexn_decode_error_skips_handlers()
{
    // Just a tag byte with no value: valid tag for field 1 varint,
    // but no bytes left to decode the varint value.
    static const uint8_t truncated[] = { 0x08 };

    ppb::reader<OneFieldSchema> r(truncated, sizeof(truncated));

    int handler_calls = 0;
    CHECK(r.lexn({},
              ppb::on<1>(
                  [&](const ppb_field &) -> ppb_error
                  {
                      handler_calls++;
                      return PPB_OK;
                  })) == PPB_ERROR_TRUNCATED_DATA);
    CHECK(handler_calls == 0);
    CHECK(r.error() == PPB_ERROR_TRUNCATED_DATA);
}

// lex_all: drains the entire input with repeated lexn batches.

static void
test_lex_all_multi_batch()
{
    int field1_calls = 0;
    int field2_calls = 0;

    ppb::reader<LexnSchema> r(lexn_two_batches_wire, sizeof(lexn_two_batches_wire));

    CHECK(r.lex_all(ppb::limit {},
              ppb::on<1>(
                  [&](const ppb_field &) -> ppb_error
                  {
                      field1_calls++;
                      return PPB_OK;
                  }),
              ppb::on<2>(
                  [&](const ppb_field &) -> ppb_error
                  {
                      field2_calls++;
                      return PPB_OK;
                  })) == PPB_OK);
    CHECK(field1_calls == 2);
    CHECK(field2_calls == 2);
    CHECK(r.empty());
    CHECK(r.error() == PPB_OK);
}

static void
test_lex_all_empty_input()
{
    ppb::reader<LexnSchema> r(nullptr, 0);

    int called = 0;
    CHECK(r.lex_all(ppb::limit {},
              ppb::on<1>(
                  [&](const ppb_field &) -> ppb_error
                  {
                      called++;
                      return PPB_OK;
                  })) == PPB_OK);
    CHECK(called == 0);
    CHECK(r.error() == PPB_OK);
}

// Zero byte or field bounds forbid any progress: lex_all must return
// immediately (no handler calls, no consumption, no infinite loop).
static void
test_lex_all_zero_bounds()
{
    int called = 0;
    auto handler = ppb::on<1>(
        [&](const ppb_field &) -> ppb_error
        {
            called++;
            return PPB_OK;
        });

    ppb::reader<LexnSchema> r(lexn_two_batches_wire, sizeof(lexn_two_batches_wire));
    CHECK(r.lex_all(ppb::limit::soft(0), handler) == PPB_OK);
    CHECK(r.size() == sizeof(lexn_two_batches_wire));

    CHECK(r.lex_all(ppb::limit::max_fields(0), handler) == PPB_OK);
    CHECK(r.size() == sizeof(lexn_two_batches_wire));

    CHECK(called == 0);
    CHECK(r.error() == PPB_OK);
}

static void
test_lex_all_handler_error()
{
    int field1_calls = 0;
    int field2_calls = 0;

    ppb::reader<LexnSchema> r(lexn_two_batches_wire, sizeof(lexn_two_batches_wire));

    CHECK(r.lex_all(ppb::limit {},
              ppb::on<1>(
                  [&](const ppb_field &) -> ppb_error
                  {
                      field1_calls++;
                      return PPB_ERROR_CORRUPT_TAG;
                  }),
              ppb::on<2>(
                  [&](const ppb_field &) -> ppb_error
                  {
                      field2_calls++;
                      return PPB_ERROR_TRUNCATED_DATA;
                  })) == PPB_ERROR_CORRUPT_TAG);
    // Both handlers in the first batch must still fire.
    CHECK(field1_calls == 1);
    CHECK(field2_calls == 1);
    // Input span points past the batch whose handler errored.
    CHECK(r.input().data() == reinterpret_cast<const std::byte *>(lexn_two_batches_wire) + 6);
    CHECK(r.input().size() == 6);
    CHECK(r.error() == PPB_ERROR_CORRUPT_TAG);
}

// lexn decode error in lex_all bails before invoking any handlers.
static void
test_lex_all_decode_error()
{
    static const uint8_t truncated[] = { 0x08 };

    ppb::reader<OneFieldSchema> r(truncated, sizeof(truncated));

    int handler_calls = 0;
    CHECK(r.lex_all(ppb::limit {},
              ppb::on<1>(
                  [&](const ppb_field &) -> ppb_error
                  {
                      handler_calls++;
                      return PPB_OK;
                  })) == PPB_ERROR_TRUNCATED_DATA);
    CHECK(handler_calls == 0);
    CHECK(r.error() == PPB_ERROR_TRUNCATED_DATA);
}

// lex_all applies its `limit` per batch, not cumulatively: with
// max_fields(1), each internal lexn dispatches at most one field
// while lex_all keeps iterating until the input drains.
static void
test_lex_all_per_batch_limit()
{
    int field1_calls = 0;
    int field2_calls = 0;

    ppb::reader<LexnSchema> r(lexn_two_batches_wire, sizeof(lexn_two_batches_wire));

    CHECK(r.lex_all(ppb::limit::max_fields(1),
              ppb::on<1>(
                  [&](const ppb_field &) -> ppb_error
                  {
                      field1_calls++;
                      return PPB_OK;
                  }),
              ppb::on<2>(
                  [&](const ppb_field &) -> ppb_error
                  {
                      field2_calls++;
                      return PPB_OK;
                  })) == PPB_OK);

    // All four fields dispatched; a cumulative limit would stop after one.
    CHECK(field1_calls == 2);
    CHECK(field2_calls == 2);
    CHECK(r.empty());
    CHECK(r.error() == PPB_OK);
}

// lex_all on an already-errored reader short-circuits.
static void
test_lex_all_sticky_error()
{
    ppb::reader<LexnSchema> r(lexn_two_batches_wire, sizeof(lexn_two_batches_wire));

    // First, trigger an error via a handler.
    (void)r.lexn({}, ppb::on<1>([&](const ppb_field &) -> ppb_error { return PPB_ERROR_CORRUPT_TAG; }));
    CHECK(r.error() == PPB_ERROR_CORRUPT_TAG);

    // lex_all should short-circuit.
    int called = 0;
    CHECK(r.lex_all(ppb::limit {},
              ppb::on<1>(
                  [&](const ppb_field &) -> ppb_error
                  {
                      called++;
                      return PPB_OK;
                  })) == PPB_ERROR_CORRUPT_TAG);
    CHECK(called == 0);
}

// parse(): prescan -> init(const reader&) -> lex+dispatch in batches.
//
// Reuses LexnSchema / lexn_two_batches_wire (two `{varint<1>, len<2>}`
// batches) so we can observe per-batch dispatch and partial consume.

static void
test_reader_parse_happy_path()
{
    int init_calls = 0;
    int field1_calls = 0;
    int field2_calls = 0;

    ppb::reader<LexnSchema> r(lexn_two_batches_wire, sizeof(lexn_two_batches_wire));

    CHECK(r.parse(
              [&](const ppb::reader<LexnSchema> &snapshot) -> ppb_error
              {
                  init_calls++;
                  // After prescan, meta() reflects what was found on the wire.
                  CHECK(snapshot.meta<1>().num_occurrences == 2);
                  CHECK(snapshot.meta<2>().num_occurrences == 2);
                  return PPB_OK;
              },
              {},
              ppb::on<1>(
                  [&](const ppb_field &) -> ppb_error
                  {
                      field1_calls++;
                      return PPB_OK;
                  }),
              ppb::on<2>(
                  [&](const ppb_field &) -> ppb_error
                  {
                      field2_calls++;
                      return PPB_OK;
                  })) == PPB_OK);

    CHECK(init_calls == 1);
    CHECK(field1_calls == 2);
    CHECK(field2_calls == 2);
    CHECK(r.error() == PPB_OK);
    CHECK(r.empty());

    // After parse(), metadata from prescan is still visible.
    CHECK(r.meta<1>().num_occurrences == 2);
    CHECK(r.meta<2>().num_occurrences == 2);

    // After reset_fields(), metadata is zeroed.
    r.reset_fields();
    CHECK(r.meta<1>().num_occurrences == 0);
    CHECK(r.meta<2>().num_occurrences == 0);
}

static void
test_reader_parse_init_error_does_not_consume()
{
    int init_calls = 0;
    int handler_calls = 0;

    ppb::reader<LexnSchema> r(lexn_two_batches_wire, sizeof(lexn_two_batches_wire));
    const size_t initial_size = r.size();

    CHECK(r.parse(
              [&](const ppb::reader<LexnSchema> &) -> ppb_error
              {
                  init_calls++;
                  return PPB_ERROR_TRUNCATED_DATA;
              },
              {},
              ppb::on<1>(
                  [&](const ppb_field &) -> ppb_error
                  {
                      handler_calls++;
                      return PPB_OK;
                  })) == PPB_ERROR_TRUNCATED_DATA);

    CHECK(init_calls == 1);
    CHECK(handler_calls == 0);
    CHECK(r.error() == PPB_ERROR_TRUNCATED_DATA);
    CHECK(r.size() == initial_size);
}

// Wire with a corrupt tag mid-stream: prescan fails, init must not run.
static const uint8_t parse_corrupt_wire[] = {
    0x08, 0x01,  /* field 1 varint 1 */
    0x00,        /* zero tag (corrupt) */
};

static void
test_reader_parse_prescan_error_skips_init()
{
    int init_calls = 0;
    int handler_calls = 0;

    ppb::reader<LexnSchema> r(parse_corrupt_wire, sizeof(parse_corrupt_wire));

    CHECK(r.parse(
              [&](const ppb::reader<LexnSchema> &) -> ppb_error
              {
                  init_calls++;
                  return PPB_OK;
              },
              {},
              ppb::on<1>(
                  [&](const ppb_field &) -> ppb_error
                  {
                      handler_calls++;
                      return PPB_OK;
                  })) == PPB_ERROR_CORRUPT_TAG);

    CHECK(init_calls == 0);
    CHECK(handler_calls == 0);
    CHECK(r.error() == PPB_ERROR_CORRUPT_TAG);
}

static void
test_reader_parse_handler_error_runs_full_batch()
{
    int field1_calls = 0;
    int field2_calls = 0;

    ppb::reader<LexnSchema> r(lexn_two_batches_wire, sizeof(lexn_two_batches_wire));

    // Field 1's handler errors on the first batch, but field 2's
    // handler in the same batch must still fire.  parse exits before
    // starting the second batch.
    CHECK(r.parse([](const ppb::reader<LexnSchema> &) -> ppb_error { return PPB_OK; }, {},
              ppb::on<1>(
                  [&](const ppb_field &) -> ppb_error
                  {
                      field1_calls++;
                      return PPB_ERROR_CORRUPT_TAG;
                  }),
              ppb::on<2>(
                  [&](const ppb_field &) -> ppb_error
                  {
                      field2_calls++;
                      return PPB_OK;
                  })) == PPB_ERROR_CORRUPT_TAG);

    CHECK(field1_calls == 1);
    CHECK(field2_calls == 1);
    CHECK(r.error() == PPB_ERROR_CORRUPT_TAG);
}

static void
test_reader_parse_sticky_error_short_circuits()
{
    int init_calls = 0;

    ppb::reader<LexnSchema> r(parse_corrupt_wire, sizeof(parse_corrupt_wire));

    // First parse hits the corrupt tag during prescan.
    CHECK(r.parse([](const ppb::reader<LexnSchema> &) -> ppb_error { return PPB_OK; }) ==
        PPB_ERROR_CORRUPT_TAG);

    // Second parse must short-circuit on the sticky error before even
    // running init.
    CHECK(r.parse(
              [&](const ppb::reader<LexnSchema> &) -> ppb_error
              {
                  init_calls++;
                  return PPB_OK;
              }) == PPB_ERROR_CORRUPT_TAG);
    CHECK(init_calls == 0);
}

// New parse() overloads: bare handlers, limit+handlers, init+handlers,
// and limit+init+handlers.

static void
test_parse_bare_handlers()
{
    int field1_calls = 0;
    int field2_calls = 0;

    ppb::reader<LexnSchema> r(lexn_two_batches_wire, sizeof(lexn_two_batches_wire));

    CHECK(r.parse(ppb::on<1>(
                      [&](const ppb_field &) -> ppb_error
                      {
                          field1_calls++;
                          return PPB_OK;
                      }),
              ppb::on<2>(
                  [&](const ppb_field &) -> ppb_error
                  {
                      field2_calls++;
                      return PPB_OK;
                  })) == PPB_OK);

    CHECK(field1_calls == 2);
    CHECK(field2_calls == 2);
    CHECK(r.empty());
}

static void
test_parse_limit_and_handlers()
{
    int field1_calls = 0;

    ppb::reader<LexnSchema> r(lexn_two_batches_wire, sizeof(lexn_two_batches_wire));

    // Cap at one field: prescan will only see the first field.
    CHECK(r.parse(ppb::limit::max_fields(1),
              ppb::on<1>(
                  [&](const ppb_field &) -> ppb_error
                  {
                      field1_calls++;
                      return PPB_OK;
                  })) == PPB_OK);

    CHECK(field1_calls == 1);
    CHECK(r.error() == PPB_OK);
}

static void
test_parse_init_and_handlers()
{
    int init_calls = 0;
    int field1_calls = 0;
    int field2_calls = 0;

    ppb::reader<LexnSchema> r(lexn_two_batches_wire, sizeof(lexn_two_batches_wire));

    CHECK(r.parse(
              [&](const ppb::reader<LexnSchema> &snapshot) -> ppb_error
              {
                  init_calls++;
                  CHECK(snapshot.meta<1>().num_occurrences == 2);
                  return PPB_OK;
              },
              ppb::on<1>(
                  [&](const ppb_field &) -> ppb_error
                  {
                      field1_calls++;
                      return PPB_OK;
                  }),
              ppb::on<2>(
                  [&](const ppb_field &) -> ppb_error
                  {
                      field2_calls++;
                      return PPB_OK;
                  })) == PPB_OK);

    CHECK(init_calls == 1);
    CHECK(field1_calls == 2);
    CHECK(field2_calls == 2);
    CHECK(r.empty());
}

static void
test_parse_limit_init_and_handlers()
{
    int init_calls = 0;
    int field1_calls = 0;

    ppb::reader<LexnSchema> r(lexn_two_batches_wire, sizeof(lexn_two_batches_wire));

    // Limit-first, then init, then handler.
    CHECK(r.parse(
              ppb::limit::hard(sizeof(lexn_two_batches_wire)),
              [&](const ppb::reader<LexnSchema> &snapshot) -> ppb_error
              {
                  init_calls++;
                  CHECK(snapshot.meta<1>().num_occurrences == 2);
                  return PPB_OK;
              },
              ppb::on<1>(
                  [&](const ppb_field &) -> ppb_error
                  {
                      field1_calls++;
                      return PPB_OK;
                  })) == PPB_OK);

    CHECK(init_calls == 1);
    CHECK(field1_calls == 2);
    CHECK(r.empty());
}

// Schema with fields A=varint<1> and B=varint<2>.  The wire has field A
// repeated 4x, then field B once, then field A again.  Since A, B, A is
// non-monotonic (A < B), lexn splits this into two batches:
//   batch 1: 4xA, 1xB  -> both handlers fire
//   batch 2: 1xA       -> only A's handler fires
// This shows parse() invokes handlers per lexn batch, not on everything
// prescan saw (which would be a single merged invocation).

using AB_Schema = ppb::schema<ppb::varint<1>, ppb::varint<2>>;

static const uint8_t a_a_a_a_b_a_wire[] = {
    0x08, 0x01,  /* field 1 varint 1 */
    0x08, 0x01,  /* field 1 varint 1 */
    0x08, 0x01,  /* field 1 varint 1 */
    0x08, 0x01,  /* field 1 varint 1 */
    0x10, 0x2a,  /* field 2 varint 42 */
    0x08, 0x09,  /* field 1 varint 9 */
};

static void
test_reader_parse_per_batch_dispatch()
{
    int field1_calls = 0;
    int field2_calls = 0;

    ppb::reader<AB_Schema> r(a_a_a_a_b_a_wire, sizeof(a_a_a_a_b_a_wire));

    CHECK(r.parse(
              [](const ppb::reader<AB_Schema> &snapshot) -> ppb_error
              {
                  // Prescan sees all occurrences.
                  CHECK(snapshot.meta<1>().num_occurrences == 5);
                  CHECK(snapshot.meta<2>().num_occurrences == 1);
                  return PPB_OK;
              },
              {},
              ppb::on<1>(
                  [&](const ppb_field &) -> ppb_error
                  {
                      field1_calls++;
                      return PPB_OK;
                  }),
              ppb::on<2>(
                  [&](const ppb_field &) -> ppb_error
                  {
                      field2_calls++;
                      return PPB_OK;
                  })) == PPB_OK);

    // Field 1 is repeated (5x), so lexn dispatches it one-at-a-time.
    // Field 2 appears once and is dispatched exactly once.
    // This confirms parse() dispatches per lexn batch, not on everything prescan saw.
    CHECK(field1_calls == 5);
    CHECK(field2_calls == 1);
    CHECK(r.empty());
}

// error semantics: field present on wire -> CORRUPT_TAG, init not called.
static void
test_reader_parse_semantics_error_field_present()
{
    using S = ppb::schema<ppb::varint<1, ppb::field_semantics::error>>;
    static const uint8_t wire[] = { 0x08, 0x01 };  // field 1 varint 1
    ppb::reader<S> r(wire, sizeof(wire));

    int init_calls = 0;

    CHECK(r.parse(
              [&](const ppb::reader<S> &) -> ppb_error
              {
                  init_calls++;
                  return PPB_OK;
              }) == PPB_ERROR_CORRUPT_TAG);
    CHECK(init_calls == 0);
    CHECK(r.error() == PPB_ERROR_CORRUPT_TAG);
    // tag = field_number * 8 + wire_type = 1*8 + 0 (varint) = 8
    CHECK(r.error_field().has_value());
    CHECK(r.error_field().value() == 8);
}

// field with error semantics NOT on wire: parse succeeds, init called.
static void
test_reader_parse_semantics_error_field_absent()
{
    using S = ppb::schema<ppb::varint<1, ppb::field_semantics::error>>;
    static const uint8_t wire[] = { 0x10, 0x01 };  // field 2 varint 1 (not in schema)
    ppb::reader<S> r(wire, sizeof(wire));

    int init_calls = 0;

    CHECK(r.parse(
              [&](const ppb::reader<S> &) -> ppb_error
              {
                  init_calls++;
                  return PPB_OK;
              }) == PPB_OK);
    CHECK(init_calls == 1);
    CHECK(r.empty());
    CHECK(!r.error_field().has_value());
}

// repeated, single occurrence: no lexn needed, handler called once.
static void
test_reader_parse_semantics_repeated_single()
{
    using S = ppb::schema<ppb::varint<1, ppb::field_semantics::repeated>>;
    static const uint8_t wire[] = { 0x08, 0x2a };  // field 1 varint 42
    ppb::reader<S> r(wire, sizeof(wire));

    int calls = 0;

    CHECK(r.parse([](const ppb::reader<S> &) -> ppb_error { return PPB_OK; }, {},
              ppb::on<1>(
                  [&](const ppb_field &) -> ppb_error
                  {
                      calls++;
                      return PPB_OK;
                  })) == PPB_OK);
    CHECK(calls == 1);
    CHECK(r.empty());
}

// repeated, multiple occurrences: forces lexn, handler called per occurrence.
static void
test_reader_parse_semantics_repeated_multi()
{
    using S = ppb::schema<ppb::varint<1, ppb::field_semantics::repeated>>;
    static const uint8_t wire[] = {
        0x08, 0x01,  /* field 1 varint 1 */
        0x08, 0x02,  /* field 1 varint 2 */
    };
    ppb::reader<S> r(wire, sizeof(wire));

    int calls = 0;

    CHECK(r.parse([](const ppb::reader<S> &) -> ppb_error { return PPB_OK; }, {},
              ppb::on<1>(
                  [&](const ppb_field &) -> ppb_error
                  {
                      calls++;
                      return PPB_OK;
                  })) == PPB_OK);
    // Repeated field -> one lexn call per occurrence.
    CHECK(calls == 2);
    CHECK(r.empty());
}

// last_write_wins, multiple occurrences: never forces lexn, handler sees
// the prescan aggregate (single call).
static void
test_reader_parse_semantics_lww_multi()
{
    using S = ppb::schema<ppb::varint<1, ppb::field_semantics::last_write_wins>>;
    static const uint8_t wire[] = {
        0x08, 0x01,  /* field 1 varint 1 */
        0x08, 0x02,  /* field 1 varint 2 */
    };
    ppb::reader<S> r(wire, sizeof(wire));

    int calls = 0;

    CHECK(r.parse([](const ppb::reader<S> &) -> ppb_error { return PPB_OK; }, {},
              ppb::on<1>(
                  [&](const ppb_field &) -> ppb_error
                  {
                      calls++;
                      return PPB_OK;
                  })) == PPB_OK);
    CHECK(calls == 1);  // no lexn -> LWW
    CHECK(r.empty());
}

// proto3_zero_default, field absent: handler called with zero-filled value.
static void
test_reader_parse_semantics_proto3_zero_default_absent()
{
    using S = ppb::schema<ppb::uint64<1, ppb::field_semantics::proto3_zero_default>>;
    static const uint8_t wire[] = {
        0x10, 0x01,  /* field 2 varint 1 (not field 1) */
    };
    ppb::reader<S> r(wire, sizeof(wire));

    int calls = 0;
    uint64_t seen = 99;

    CHECK(r.parse([](const ppb::reader<S> &) -> ppb_error { return PPB_OK; }, {},
              ppb::on<1>(
                  [&](uint64_t v) -> ppb_error
                  {
                      calls++;
                      seen = v;
                      return PPB_OK;
                  })) == PPB_OK);
    CHECK(calls == 1);  // absent field still dispatched
    CHECK(seen == 0);   // zero-filled default
    CHECK(r.empty());
}

// proto3_zero_default, field present: handler called with wire value.
static void
test_reader_parse_semantics_proto3_zero_default_present()
{
    using S = ppb::schema<ppb::uint64<1, ppb::field_semantics::proto3_zero_default>>;
    static const uint8_t wire[] = {
        0x08, 0x2a,  /* field 1 varint 42 */
    };
    ppb::reader<S> r(wire, sizeof(wire));

    int calls = 0;
    uint64_t seen = 0;

    CHECK(r.parse([](const ppb::reader<S> &) -> ppb_error { return PPB_OK; }, {},
              ppb::on<1>(
                  [&](uint64_t v) -> ppb_error
                  {
                      calls++;
                      seen = v;
                      return PPB_OK;
                  })) == PPB_OK);
    CHECK(calls == 1);
    CHECK(seen == 42);
    CHECK(r.empty());
}

// proto3_zero_default, multiple occurrences: no lexn forced, handler sees
// last-write-wins aggregate (single call with last value).
static void
test_reader_parse_semantics_proto3_zero_default_multi()
{
    using S = ppb::schema<ppb::uint64<1, ppb::field_semantics::proto3_zero_default>>;
    static const uint8_t wire[] = {
        0x08, 0x01,  /* field 1 varint 1 */
        0x08, 0x02,  /* field 1 varint 2 */
    };
    ppb::reader<S> r(wire, sizeof(wire));

    int calls = 0;

    CHECK(r.parse([](const ppb::reader<S> &) -> ppb_error { return PPB_OK; }, {},
              ppb::on<1>(
                  [&](uint64_t) -> ppb_error
                  {
                      calls++;
                      return PPB_OK;
                  })) == PPB_OK);
    CHECK(calls == 1);  // no lexn -> single dispatch
    CHECK(r.empty());
}

// proto3_zero_default with a LEN field absent: handler receives empty span.
static void
test_reader_parse_semantics_proto3_zero_default_len_absent()
{
    using S = ppb::schema<ppb::bytes<1, std::byte, ppb::field_semantics::proto3_zero_default>>;
    static const uint8_t wire[] = {
        0x10, 0x01,  /* field 2 varint 1 (not field 1) */
    };
    ppb::reader<S> r(wire, sizeof(wire));

    int calls = 0;
    size_t seen_size = 99;

    CHECK(r.parse([](const ppb::reader<S> &) -> ppb_error { return PPB_OK; }, {},
              ppb::on<1>(
                  [&](std::span<const std::byte> v) -> ppb_error
                  {
                      calls++;
                      seen_size = v.size();
                      return PPB_OK;
                  })) == PPB_OK);
    CHECK(calls == 1);        // absent field still dispatched
    CHECK(seen_size == 0);    // empty span for absent LEN
    CHECK(r.empty());
}

// proto3_zero_default in a schema with a repeated field: lexn is forced by
// the repeated field; proto3_zero_default behaves like last_write_wins
// (absent field not dispatched in the lexn path).
static void
test_reader_parse_semantics_proto3_zero_default_in_lexn_path()
{
    using S = ppb::schema<ppb::uint64<1, ppb::field_semantics::proto3_zero_default>,
        ppb::varint<2, ppb::field_semantics::repeated>>;
    static const uint8_t wire[] = {
        0x10, 0x01,  /* field 2 varint 1 */
        0x10, 0x02,  /* field 2 varint 2 (forces lexn) */
    };
    ppb::reader<S> r(wire, sizeof(wire));

    int calls_f1 = 0;
    int calls_f2 = 0;

    CHECK(r.parse([](const ppb::reader<S> &) -> ppb_error { return PPB_OK; }, {},
              ppb::on<1>(
                  [&](uint64_t) -> ppb_error
                  {
                      calls_f1++;
                      return PPB_OK;
                  }),
              ppb::on<2>(
                  [&](const ppb_field &) -> ppb_error
                  {
                      calls_f2++;
                      return PPB_OK;
                  })) == PPB_OK);
    CHECK(calls_f1 == 0);  // absent + lexn path -> not dispatched (same as LWW)
    CHECK(calls_f2 == 2);
    CHECK(r.empty());
}

// proto3_zero_default via prescan(), field absent: handler called with zero.
static void
test_reader_prescan_semantics_proto3_zero_default_absent()
{
    using S = ppb::schema<ppb::uint64<1, ppb::field_semantics::proto3_zero_default>>;
    static const uint8_t wire[] = {
        0x10, 0x01,  /* field 2 varint 1 (not field 1) */
    };
    ppb::reader<S> r(wire, sizeof(wire));

    int calls = 0;
    uint64_t seen = 99;

    CHECK(r.prescan({},
              ppb::on<1>(
                  [&](uint64_t v) -> ppb_error
                  {
                      calls++;
                      seen = v;
                      return PPB_OK;
                  })) >= 0);
    CHECK(calls == 1);  // absent field dispatched via prescan
    CHECK(seen == 0);   // zero-filled default
    CHECK(r.error() == PPB_OK);
}

// proto3_zero_default via prescan(), field present: handler called with wire value.
static void
test_reader_prescan_semantics_proto3_zero_default_present()
{
    using S = ppb::schema<ppb::uint64<1, ppb::field_semantics::proto3_zero_default>>;
    static const uint8_t wire[] = {
        0x08, 0x2a,  /* field 1 varint 42 */
    };
    ppb::reader<S> r(wire, sizeof(wire));

    int calls = 0;
    uint64_t seen = 0;

    CHECK(r.prescan({},
              ppb::on<1>(
                  [&](uint64_t v) -> ppb_error
                  {
                      calls++;
                      seen = v;
                      return PPB_OK;
                  })) >= 0);
    CHECK(calls == 1);
    CHECK(seen == 42);
    CHECK(r.error() == PPB_OK);
}

// last_write_wins via prescan(), field absent: handler not called.
static void
test_reader_prescan_semantics_lww_absent()
{
    using S = ppb::schema<ppb::uint64<1, ppb::field_semantics::last_write_wins>>;
    static const uint8_t wire[] = {
        0x10, 0x01,  /* field 2 varint 1 (not field 1) */
    };
    ppb::reader<S> r(wire, sizeof(wire));

    int calls = 0;

    CHECK(r.prescan({},
              ppb::on<1>(
                  [&](uint64_t) -> ppb_error
                  {
                      calls++;
                      return PPB_OK;
                  })) >= 0);
    CHECK(calls == 0);  // absent LWW -> not dispatched
    CHECK(r.error() == PPB_OK);
}

// last_write_wins + proto3_zero_default via prescan(), both absent:
// proto3 fires with zero, LWW stays silent.
static void
test_reader_prescan_semantics_lww_and_proto3_absent()
{
    using S = ppb::schema<ppb::uint64<1, ppb::field_semantics::last_write_wins>,
        ppb::uint64<2, ppb::field_semantics::proto3_zero_default>>;
    static const uint8_t wire[] = {
        0x18, 0x01,  /* field 3 varint 1 (neither field 1 nor field 2) */
    };
    ppb::reader<S> r(wire, sizeof(wire));

    int calls_lww = 0;
    int calls_p3 = 0;

    CHECK(r.prescan({},
              ppb::on<1>(
                  [&](uint64_t) -> ppb_error
                  {
                      calls_lww++;
                      return PPB_OK;
                  }),
              ppb::on<2>(
                  [&](uint64_t) -> ppb_error
                  {
                      calls_p3++;
                      return PPB_OK;
                  })) >= 0);
    CHECK(calls_lww == 0);  // absent LWW -> silent
    CHECK(calls_p3 == 1);   // absent proto3 -> fires with zero
    CHECK(r.error() == PPB_OK);
}

// singular, multiple occurrences with distinct values: lost_distinct_u64
// forces lexn, handler called per occurrence.
static void
test_reader_parse_semantics_singular_distinct()
{
    using S = ppb::schema<ppb::varint<1, ppb::field_semantics::singular>>;
    static const uint8_t wire[] = {
        0x08, 0x01,  /* field 1 varint 1 */
        0x08, 0x02,  /* field 1 varint 2 (distinct) */
    };
    ppb::reader<S> r(wire, sizeof(wire));

    int calls = 0;

    CHECK(r.parse([](const ppb::reader<S> &) -> ppb_error { return PPB_OK; }, {},
              ppb::on<1>(
                  [&](const ppb_field &) -> ppb_error
                  {
                      calls++;
                      return PPB_OK;
                  })) == PPB_OK);
    CHECK(calls == 2);  // distinct values -> lexn forced -> per-occurrence
    CHECK(r.empty());
}

// singular, multiple occurrences all equal: lost_distinct_u64 stays 0,
// no lexn needed, handler called once.
static void
test_reader_parse_semantics_singular_equal()
{
    using S = ppb::schema<ppb::varint<1, ppb::field_semantics::singular>>;
    static const uint8_t wire[] = {
        0x08, 0x01,  /* field 1 varint 1 */
        0x08, 0x01,  /* field 1 varint 1 (same value) */
    };
    ppb::reader<S> r(wire, sizeof(wire));

    int calls = 0;

    CHECK(r.parse([](const ppb::reader<S> &) -> ppb_error { return PPB_OK; }, {},
              ppb::on<1>(
                  [&](const ppb_field &) -> ppb_error
                  {
                      calls++;
                      return PPB_OK;
                  })) == PPB_OK);
    CHECK(calls == 1);  // equal values -> no lexn
    CHECK(r.empty());
}

// singular LEN, multiple occurrences: wire type LEN always forces lexn,
// regardless of lost_distinct_u64 (which is always 0 for LEN).
static void
test_reader_parse_semantics_singular_len_multi()
{
    using S = ppb::schema<ppb::len<1, ppb::field_semantics::singular>>;
    static const uint8_t wire[] = {
        0x0a, 0x01, 0x42,  /* field 1 LEN "B" */
        0x0a, 0x01, 0x42,  /* field 1 LEN "B" (same payload) */
    };
    ppb::reader<S> r(wire, sizeof(wire));

    int calls = 0;

    CHECK(r.parse([](const ppb::reader<S> &) -> ppb_error { return PPB_OK; }, {},
              ppb::on<1>(
                  [&](const ppb_field &) -> ppb_error
                  {
                      calls++;
                      return PPB_OK;
                  })) == PPB_OK);
    CHECK(calls == 2);  // LEN -> lexn forced
    CHECK(r.empty());
}

// always_lexn forces lexn, so handlers fire in wire order, not schema order.
// The wire has field 2 before field 1; lexn delivers field 2 first.
static void
test_reader_parse_semantics_always_lexn_wire_order()
{
    using S = ppb::schema<ppb::varint<1, ppb::field_semantics::repeated>,
        ppb::varint<2, ppb::field_semantics::always_lexn>>;
    static const uint8_t wire[] = {
        0x10, 0x01,  /* field 2 varint 1 (higher tag, but on the wire first) */
        0x08, 0x02,  /* field 1 varint 2 */
    };
    ppb::reader<S> r(wire, sizeof(wire));

    std::vector<int> order;

    CHECK(r.parse([](const ppb::reader<S> &) -> ppb_error { return PPB_OK; }, {},
              ppb::on<1>(
                  [&](const ppb_field &) -> ppb_error
                  {
                      order.push_back(1);
                      return PPB_OK;
                  }),
              ppb::on<2>(
                  [&](const ppb_field &) -> ppb_error
                  {
                      order.push_back(2);
                      return PPB_OK;
                  })) == PPB_OK);

    // lexn delivers in wire order: field 2 then field 1.
    CHECK(order.size() == 2);
    CHECK(order[0] == 2);
    CHECK(order[1] == 1);
    CHECK(r.empty());
}

/*
 * Higher-level field type tests: each new typed field wraps a base
 * wire-type field and adds an `extract_value` that decodes the matched
 * value into a typed result.  We verify the conversion by attaching an
 * `on<Key>` handler whose argument type matches the decoded value.
 */

static void
test_typed_scalar_varint_fields()
{
    using S = ppb::schema<ppb::int32<1>, ppb::int64<2>, ppb::sint32<3>, ppb::sint64<4>, ppb::uint32<5>,
        ppb::uint64<6>, ppb::boolean<7>>;

    static const uint8_t wire[] = {
        0x08, 0x96, 0x01,                          /* f1 int32 = 150 */
        0x10, 0xe8, 0x07,                          /* f2 int64 = 1000 */
        0x18, 0x01,                                /* f3 sint32 = -1 (zigzag 1) */
        0x20, 0x03,                                /* f4 sint64 = -2 (zigzag 3) */
        0x28, 0xff, 0xff, 0xff, 0xff, 0x0f,        /* f5 uint32 = 0xFFFFFFFF */
        0x30, 0xb9, 0x60,                          /* f6 uint64 = 12345 */
        0x38, 0x01,                                /* f7 boolean = true */
    };

    int32_t v_int32 = 0;
    int64_t v_int64 = 0;
    int32_t v_sint32 = 0;
    int64_t v_sint64 = 0;
    uint32_t v_uint32 = 0;
    uint64_t v_uint64 = 0;
    bool v_bool = false;

    ppb::reader<S> r(wire, sizeof(wire));

    CHECK(r.parse([](const ppb::reader<S> &) -> ppb_error { return PPB_OK; }, {},
              ppb::on<1>(
                  [&](int32_t v) -> ppb_error
                  {
                      v_int32 = v;
                      return PPB_OK;
                  }),
              ppb::on<2>(
                  [&](int64_t v) -> ppb_error
                  {
                      v_int64 = v;
                      return PPB_OK;
                  }),
              ppb::on<3>(
                  [&](int32_t v) -> ppb_error
                  {
                      v_sint32 = v;
                      return PPB_OK;
                  }),
              ppb::on<4>(
                  [&](int64_t v) -> ppb_error
                  {
                      v_sint64 = v;
                      return PPB_OK;
                  }),
              ppb::on<5>(
                  [&](uint32_t v) -> ppb_error
                  {
                      v_uint32 = v;
                      return PPB_OK;
                  }),
              ppb::on<6>(
                  [&](uint64_t v) -> ppb_error
                  {
                      v_uint64 = v;
                      return PPB_OK;
                  }),
              ppb::on<7>(
                  [&](bool v) -> ppb_error
                  {
                      v_bool = v;
                      return PPB_OK;
                  })) == PPB_OK);

    CHECK(v_int32 == 150);
    CHECK(v_int64 == 1000);
    CHECK(v_sint32 == -1);
    CHECK(v_sint64 == -2);
    CHECK(v_uint32 == 0xFFFFFFFFU);
    CHECK(v_uint64 == 12345);
    CHECK(v_bool == true);
}

/*
 * sint32 truncates the varint to 32 bits *before* un-zigzagging, to
 * match the protobuf reference on non-canonical (over-long) encodings.
 * The scalar and packed paths must agree when handed a varint >= 2^32.
 */
static void
test_sint32_noncanonical_truncates()
{
    /* Singular sint32 (field 3, tag 0x18). */
    {
        using S = ppb::schema<ppb::sint32<3>>;
        static const uint8_t wire[] = { 0x18, 0x81, 0x80, 0x80, 0x80, 0x10 };

        int32_t v = 0;
        ppb::reader<S> r(wire, sizeof(wire));
        CHECK(r.parse(ppb::on<3>(
                  [&](int32_t x) -> ppb_error
                  {
                      v = x;
                      return PPB_OK;
                  })) == PPB_OK);
        CHECK(v == -1);
    }

    /*
     * Packed sint32 (field 2, tag 0x12): payload [wide-varint, 0x08] -> [-1, 4].
     * The second element (varint 8) has no high bits, so truncation is a no-op.
     */
    {
        using S = ppb::schema<ppb::packed_sint32<2>>;
        static const uint8_t wire[] = { 0x12, 0x06, 0x81, 0x80, 0x80, 0x80, 0x10, 0x08 };

        std::vector<int32_t> vs;
        ppb::reader<S> r(wire, sizeof(wire));
        CHECK(r.parse(ppb::on<2>(
                  [&](auto view) -> ppb_error
                  {
                      for (auto x : view)
                      {
                          vs.push_back(x);
                      }

                      return PPB_OK;
                  })) == PPB_OK);
        CHECK(vs.size() == 2);
        CHECK(vs[0] == -1);
        CHECK(vs[1] == 4);
    }
}

static void
test_typed_int32_negative_decodes_as_10_byte_varint()
{
    using S = ppb::schema<ppb::int32<1>>;

    static const uint8_t wire[] = {
        0x08, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01, /* f1 int32 = -1 (10-byte varint) */
    };

    int32_t v = 0;
    ppb::reader<S> r(wire, sizeof(wire));

    CHECK(r.parse([](const ppb::reader<S> &) -> ppb_error { return PPB_OK; }, {},
              ppb::on<1>(
                  [&](int32_t x) -> ppb_error
                  {
                      v = x;
                      return PPB_OK;
                  })) == PPB_OK);
    CHECK(v == -1);
}

enum class Color : uint32_t
{
    red = 1,
    green = 2,
    blue = 3,
};

static void
test_typed_enumerated_field()
{
    using S = ppb::schema<ppb::enumerated<1, Color>>;

    static const uint8_t wire[] = {
        0x08, 0x02, /* f1 = 2 (green) */
    };

    Color v = Color::red;
    ppb::reader<S> r(wire, sizeof(wire));

    CHECK(r.parse([](const ppb::reader<S> &) -> ppb_error { return PPB_OK; }, {},
              ppb::on<1>(
                  [&](Color c) -> ppb_error
                  {
                      v = c;
                      return PPB_OK;
                  })) == PPB_OK);
    CHECK(v == Color::green);
}

static void
test_typed_scalar_i32_fields()
{
    using S = ppb::schema<ppb::fixed32<1>, ppb::sfixed32<2>, ppb::f32<3>>;

    static const uint8_t wire[] = {
        0x0d, 0xef, 0xbe, 0xad, 0xde, /* f1 fixed32 = 0xDEADBEEF */
        0x15, 0xfe, 0xff, 0xff, 0xff, /* f2 sfixed32 = -2 */
        0x1d, 0x00, 0x00, 0xc0, 0x3f, /* f3 f32 = 1.5f */
    };

    uint32_t v_fixed = 0;
    int32_t v_sfixed = 0;
    float v_f = 0.0f;

    ppb::reader<S> r(wire, sizeof(wire));

    CHECK(r.parse([](const ppb::reader<S> &) -> ppb_error { return PPB_OK; }, {},
              ppb::on<1>(
                  [&](uint32_t v) -> ppb_error
                  {
                      v_fixed = v;
                      return PPB_OK;
                  }),
              ppb::on<2>(
                  [&](int32_t v) -> ppb_error
                  {
                      v_sfixed = v;
                      return PPB_OK;
                  }),
              ppb::on<3>(
                  [&](float v) -> ppb_error
                  {
                      v_f = v;
                      return PPB_OK;
                  })) == PPB_OK);

    CHECK(v_fixed == 0xDEADBEEFU);
    CHECK(v_sfixed == -2);
    CHECK(v_f == 1.5f);
}

static void
test_typed_scalar_i64_fields()
{
    using S = ppb::schema<ppb::fixed64<1>, ppb::sfixed64<2>, ppb::f64<3>>;

    static const uint8_t wire[] = {
        0x09, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, /* f1 fixed64 */
        0x11, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* f2 sfixed64 = -1 */
        0x19, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x40, /* f3 f64 = 2.5 */
    };

    uint64_t v_fixed = 0;
    int64_t v_sfixed = 0;
    double v_f = 0.0;

    ppb::reader<S> r(wire, sizeof(wire));

    CHECK(r.parse([](const ppb::reader<S> &) -> ppb_error { return PPB_OK; }, {},
              ppb::on<1>(
                  [&](uint64_t v) -> ppb_error
                  {
                      v_fixed = v;
                      return PPB_OK;
                  }),
              ppb::on<2>(
                  [&](int64_t v) -> ppb_error
                  {
                      v_sfixed = v;
                      return PPB_OK;
                  }),
              ppb::on<3>(
                  [&](double v) -> ppb_error
                  {
                      v_f = v;
                      return PPB_OK;
                  })) == PPB_OK);

    CHECK(v_fixed == 0x1122334455667788ULL);
    CHECK(v_sfixed == -1);
    CHECK(v_f == 2.5);
}

static void
test_typed_utf8string_field()
{
    using S = ppb::schema<ppb::utf8string<1>>;

    static const uint8_t wire[] = {
        0x0a, 0x05, 'h', 'e', 'l', 'l', 'o', /* f1 utf8string = "hello" */
    };

    std::string_view v;
    ppb::reader<S> r(wire, sizeof(wire));

    CHECK(r.parse([](const ppb::reader<S> &) -> ppb_error { return PPB_OK; }, {},
              ppb::on<1>(
                  [&](std::string_view s) -> ppb_error
                  {
                      v = s;
                      return PPB_OK;
                  })) == PPB_OK);
    CHECK(v == std::string_view("hello"));
}

static void
test_typed_bytes_default_element()
{
    using S = ppb::schema<ppb::bytes<1>>;

    static const uint8_t wire[] = {
        0x0a, 0x03, 0xca, 0xfe, 0xbe, /* f1 bytes = {0xCA, 0xFE, 0xBE} */
    };

    std::span<const std::byte> v;
    ppb::reader<S> r(wire, sizeof(wire));

    CHECK(r.parse([](const ppb::reader<S> &) -> ppb_error { return PPB_OK; }, {},
              ppb::on<1>(
                  [&](std::span<const std::byte> s) -> ppb_error
                  {
                      v = s;
                      return PPB_OK;
                  })) == PPB_OK);
    CHECK(v.size() == 3);
    CHECK(static_cast<uint8_t>(v[0]) == 0xca);
    CHECK(static_cast<uint8_t>(v[1]) == 0xfe);
    CHECK(static_cast<uint8_t>(v[2]) == 0xbe);
}

static void
test_typed_bytes_uint32_element()
{
    using S = ppb::schema<ppb::packed_fixed32<1>>;

    static const uint8_t wire[] = {
        0x0a, 0x08, /* f1 bytes header (8 payload bytes) */
        0xef, 0xbe, 0xad, 0xde, /* uint32 0xDEADBEEF */
        0xbe, 0xba, 0xfe, 0xca, /* uint32 0xCAFEBABE */
    };

    std::vector<uint32_t> got;
    ppb::reader<S> r(wire, sizeof(wire));

    CHECK(r.parse([](const ppb::reader<S> &) -> ppb_error { return PPB_OK; }, {},
              ppb::on<1>(
                  [&](std::span<const ppb::le_packed<uint32_t>> s) -> ppb_error
                  {
                      for (const auto &e : s)
                      {
                          got.push_back(static_cast<uint32_t>(e));
                      }

                      return PPB_OK;
                  })) == PPB_OK);
    CHECK(got.size() == 2);
    CHECK(got[0] == 0xDEADBEEFU);
    CHECK(got[1] == 0xCAFEBABEU);
}

static void
test_typed_bytes_misaligned_size_sets_error()
{
    using S = ppb::schema<ppb::packed_fixed32<1>>;

    /* Payload size = 5 is not a multiple of sizeof(uint32_t)=4. */
    static const uint8_t wire[] = {
        0x0a, 0x05, 0x01, 0x02, 0x03, 0x04, 0x05, /* f1 bytes payload (5 bytes, misaligned) */
    };

    bool handler_called = false;
    size_t observed_size = 999;
    ppb::reader<S> r(wire, sizeof(wire));
    ppb_error rc = r.parse([](const ppb::reader<S> &) -> ppb_error { return PPB_OK; }, {},
        ppb::on<1>(
            [&](std::span<const ppb::le_packed<uint32_t>> s) -> ppb_error
            {
                handler_called = true;
                observed_size = s.size();
                return PPB_OK;
            }));

    /* The handler runs with an empty span, but the reader records the error. */
    CHECK(rc == PPB_ERROR_TRUNCATED_DATA);
    CHECK(handler_called);
    CHECK(observed_size == 0);
    CHECK(r.error() == PPB_ERROR_TRUNCATED_DATA);
}

static void
test_packed_fixed32_misaligned_error_semantics_sticky()
{
    /*
     * Schema order: error-semantics field then packed_fixed32.
     * Both fields appear on the wire.  dispatch_tuple visits in
     * schema order, so the error handler fires first — CORRUPT_TAG
     * sticks, and the packed field's extract_value (which would
     * set TRUNCATED_DATA) is silenced.
     */
    using S = ppb::schema<ppb::varint<1, ppb::field_semantics::error>, ppb::packed_fixed32<2>>;

    static const uint8_t wire[] = {
        0x08, 0x01, /* field 1 varint 1 (error semantics: present → corrupt) */
        0x12, 0x07, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, /* field 2 LEN 7 bytes */
    };

    ppb::reader<S> r(wire, sizeof(wire));

    bool packed_called = false;
    size_t observed_size = 999;

    (void)r.prescan({}, ppb::on<1>([](const ppb_field &) -> ppb_error { return PPB_ERROR_CORRUPT_TAG; }),
        ppb::on<2>(
            [&](std::span<const ppb::le_packed<uint32_t>> s) -> ppb_error
            {
                packed_called = true;
                observed_size = s.size();
                return PPB_OK;
            }));

    CHECK(packed_called);
    CHECK(observed_size == 0);
    CHECK(r.error() == PPB_ERROR_CORRUPT_TAG);
}

static void
test_typed_packed_fixed_fields()
{
    using S = ppb::schema<ppb::packed_fixed32<1>, ppb::packed_sfixed64<2>, ppb::packed_f32<3>>;

    static const uint8_t wire[] = {
        0x0a, 0x08, /* f1 packed_fixed32 header (8 bytes) */
        0xef, 0xbe, 0xad, 0xde, /* 0xDEADBEEF */
        0xbe, 0xba, 0xfe, 0xca, /* 0xCAFEBABE */
        0x12, 0x10, /* f2 packed_sfixed64 header (16 bytes) */
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* -1 */
        0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* -2 */
        0x1a, 0x08, /* f3 packed_f32 header (8 bytes) */
        0x00, 0x00, 0x80, 0x3f, /* 1.0f */
        0x00, 0x00, 0x00, 0x40, /* 2.0f */
    };

    std::vector<uint32_t> u32s;
    std::vector<int64_t> i64s;
    std::vector<float> floats;

    ppb::reader<S> r(wire, sizeof(wire));

    CHECK(r.parse([](const ppb::reader<S> &) -> ppb_error { return PPB_OK; }, {},
              ppb::on<1>(
                  [&](std::span<const ppb::le_packed<uint32_t>> s) -> ppb_error
                  {
                      for (const auto &e : s)
                      {
                          u32s.push_back(static_cast<uint32_t>(e));
                      }

                      return PPB_OK;
                  }),
              ppb::on<2>(
                  [&](std::span<const ppb::le_packed<int64_t>> s) -> ppb_error
                  {
                      for (const auto &e : s)
                      {
                          i64s.push_back(static_cast<int64_t>(e));
                      }

                      return PPB_OK;
                  }),
              ppb::on<3>(
                  [&](std::span<const ppb::le_packed<float>> s) -> ppb_error
                  {
                      for (const auto &e : s)
                      {
                          floats.push_back(static_cast<float>(e));
                      }

                      return PPB_OK;
                  })) == PPB_OK);

    CHECK(u32s.size() == 2);
    CHECK(u32s[0] == 0xDEADBEEFU);
    CHECK(u32s[1] == 0xCAFEBABEU);

    CHECK(i64s.size() == 2);
    CHECK(i64s[0] == -1);
    CHECK(i64s[1] == -2);

    CHECK(floats.size() == 2);
    CHECK(floats[0] == 1.0f);
    CHECK(floats[1] == 2.0f);
}

static void
test_typed_packed_sfixed32()
{
    /*
     * le_packed<int32_t> exercises the 4-byte bswap path in
     * le_packed<T>::value() when run on big-endian platforms.
     */
    using S = ppb::schema<ppb::packed_sfixed32<1>>;

    static const uint8_t wire[] = {
        0x0a, 0x0c, /* f1 bytes header (12 bytes) */
        0xff, 0xff, 0xff, 0xff, /* -1 */
        0xfe, 0xff, 0xff, 0xff, /* -2 */
        0x2a, 0x00, 0x00, 0x00, /* 42 */
    };

    std::vector<int32_t> got;
    ppb::reader<S> r(wire, sizeof(wire));

    CHECK(r.parse([](const ppb::reader<S> &) -> ppb_error { return PPB_OK; }, {},
              ppb::on<1>(
                  [&](std::span<const ppb::le_packed<int32_t>> s) -> ppb_error
                  {
                      for (const auto &e : s)
                      {
                          got.push_back(static_cast<int32_t>(e));
                      }

                      return PPB_OK;
                  })) == PPB_OK);
    CHECK(got.size() == 3);
    CHECK(got[0] == -1);
    CHECK(got[1] == -2);
    CHECK(got[2] == 42);
}

static void
test_typed_packed_f64()
{
    /*
     * le_packed<double> exercises the 8-byte bswap path in
     * le_packed<T>::value() when run on big-endian platforms.
     */
    using S = ppb::schema<ppb::packed_f64<1>>;

    /* IEEE 754 LE on wire: 1.0 = 0x3FF0000000000000, -1.0 = 0xBFF0000000000000. */
    static const uint8_t wire[] = {
        0x0a, 0x10, /* f1 bytes header (16 bytes) */
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf0, 0x3f, /* 1.0 */
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf0, 0xbf, /* -1.0 */
    };

    std::vector<double> got;
    ppb::reader<S> r(wire, sizeof(wire));

    CHECK(r.parse([](const ppb::reader<S> &) -> ppb_error { return PPB_OK; }, {},
              ppb::on<1>(
                  [&](std::span<const ppb::le_packed<double>> s) -> ppb_error
                  {
                      for (const auto &e : s)
                      {
                          got.push_back(static_cast<double>(e));
                      }

                      return PPB_OK;
                  })) == PPB_OK);
    CHECK(got.size() == 2);
    CHECK(got[0] == 1.0);
    CHECK(got[1] == -1.0);
}

/* Reads a little-endian UInt from p. */
template <typename UInt>
static UInt
le_assemble(const uint8_t *p)
{
    UInt v = 0;
    for (size_t i = 0; i < sizeof(UInt); i++)
    {
        v |= static_cast<UInt>(static_cast<UInt>(p[i]) << (8 * i));
    }

    return v;
}

/*
 * Runs the production `extract_value` over every start alignment and
 * multiple lengths with exact-sized heap payloads, so a
 * partial-element tail over-read trips an ASan red-zone.  The
 * per-element value checks add useful coverage on big-endian
 * platforms.
 */
template <typename Field, typename T, typename UInt>
static void
sweep_packed_fixed()
{
    static_assert(sizeof(T) == sizeof(UInt));
    constexpr size_t elt = sizeof(T);

    for (size_t pad = 0; pad < 16; pad++)
    {
        for (size_t len = 0; len <= 3 * elt + 1; len++)
        {
            /*
             * The payload occupies the last `len` bytes of an
             * exact-sized allocation, so its end coincides with the
             * allocation end and any tail over-read hits a red-zone;
             * the `pad` head bytes vary the start alignment.
             */
            const size_t total = pad + len;
            uint8_t *base = new uint8_t[total == 0 ? 1 : total];
            uint8_t *payload = base + pad;
            for (size_t i = 0; i < len; i++)
            {
                payload[i] = static_cast<uint8_t>(0x80u + i * 7u + pad);
            }

            ppb_field field {};
            field.v.payload.buf = payload;
            field.v.payload.size = len;

            ppb_error err = PPB_OK;
            std::span<const ppb::le_packed<T>> view = Field::extract_value(field, &err);

            if (len % elt != 0)
            {
                /* Partial trailing element: empty span, error, no tail read. */
                CHECK(view.size() == 0);
                CHECK(err == PPB_ERROR_TRUNCATED_DATA);
            }
            else
            {
                CHECK(err == PPB_OK);
                CHECK(view.size() == len / elt);

                size_t idx = 0;
                for (const ppb::le_packed<T> &e : view)
                {
                    T got = static_cast<T>(e); /* le_packed<T>::value(): the code under test */
                    UInt got_bits;
                    std::memcpy(&got_bits, &got, sizeof(UInt));

                    CHECK(got_bits == le_assemble<UInt>(payload + idx * elt));
                    idx++;
                }

                CHECK(idx == len / elt);
            }

            delete[] base;
        }
    }
}

static void
test_packed_fixed_alignment_length_sweep()
{
    sweep_packed_fixed<ppb::packed_fixed32<1>, uint32_t, uint32_t>();
    sweep_packed_fixed<ppb::packed_sfixed32<1>, int32_t, uint32_t>();
    sweep_packed_fixed<ppb::packed_f32<1>, float, uint32_t>();
    sweep_packed_fixed<ppb::packed_fixed64<1>, uint64_t, uint64_t>();
    sweep_packed_fixed<ppb::packed_sfixed64<1>, int64_t, uint64_t>();
    sweep_packed_fixed<ppb::packed_f64<1>, double, uint64_t>();
}

/*
 * Drive every scalar `extract_value` over adversarial 64-bit payloads
 * (all-ones, the sign bit alone, and float/double NaN/inf bit
 * patterns) and assert (in ubsan builds) that each extract is just
 * bitcast/truncation without UB due to type-punning or
 * invalid-representation issues:
 *
 *   - integer extracts are the plain (re)interpretation of the bits;
 *   - `bool` is `bits != 0`, never a punned non-0/1 object (UBSan
 *     `bool` in CI rejects an out-of-range representation);
 *   - `float`/`double` are materialised only via `bit_cast` and
 *     round-trip their exact source bits, compared bit-for-bit so a
 *     NaN payload is never lost to an arithmetic comparison;
 *   - a fixed-underlying `enum class` is total for every input: the
 *     `fixed_underlying_enum` check guarantees it, and UBSan confirms
 *     dynamically.
 */
enum class A5Key : int
{
    a = 1
};

enum class A5Color : int
{
    red = 0,
    green = 1,
    blue = 2
};

static void
test_scalar_value_punning_sweep()
{
    static const uint64_t patterns[] = {
        0,
        1,
        2,
        0xFF,
        0x100,
        0x7FFFFFFF,
        0x80000000,
        0xFFFFFFFF,
        0x7FFFFFFFFFFFFFFFull,
        0x8000000000000000ull,
        0xFFFFFFFFFFFFFFFFull,
        0x7F800001ull /* f32 sNaN */,
        0x7FC00000ull /* f32 qNaN */,
        0xFF800000ull /* f32 -inf */,
        0x7FF0000000000001ull /* f64 sNaN */,
        0x7FF8000000000000ull /* f64 qNaN */,
        0xFFF0000000000000ull /* f64 -inf */,
    };

    for (uint64_t bits : patterns)
    {
        ppb_field f {};
        f.v.u64 = bits;
        ppb_error err = PPB_OK;

        CHECK(ppb::int32<A5Key::a>::extract_value(f, &err) ==
            static_cast<int32_t>(static_cast<uint32_t>(bits)));
        CHECK(ppb::uint32<A5Key::a>::extract_value(f, &err) == static_cast<uint32_t>(bits));
        CHECK(ppb::int64<A5Key::a>::extract_value(f, &err) == static_cast<int64_t>(bits));
        CHECK(ppb::uint64<A5Key::a>::extract_value(f, &err) == bits);
        CHECK(ppb::fixed32<A5Key::a>::extract_value(f, &err) == static_cast<uint32_t>(bits));
        CHECK(ppb::fixed64<A5Key::a>::extract_value(f, &err) == bits);
        CHECK(ppb::sfixed32<A5Key::a>::extract_value(f, &err) ==
            std::bit_cast<int32_t>(static_cast<uint32_t>(bits)));
        CHECK(ppb::sfixed64<A5Key::a>::extract_value(f, &err) == std::bit_cast<int64_t>(bits));

        CHECK(ppb::boolean<A5Key::a>::extract_value(f, &err) == (bits != 0));

        const float fl = ppb::f32<A5Key::a>::extract_value(f, &err);
        CHECK(std::bit_cast<uint32_t>(fl) == static_cast<uint32_t>(bits));

        const double db = ppb::f64<A5Key::a>::extract_value(f, &err);
        CHECK(std::bit_cast<uint64_t>(db) == bits);

        const A5Color c = ppb::enumerated<A5Key::a, A5Color>::extract_value(f, &err);
        CHECK(static_cast<int>(c) == static_cast<int>(bits));

        CHECK(err == PPB_OK);
    }
}

static void
test_typed_packed_varint_fields()
{
    using S = ppb::schema<ppb::packed_int32<1>, ppb::packed_sint32<2>, ppb::packed_boolean<3>,
        ppb::packed_uint64<4>>;

    static const uint8_t wire[] = {
        0x0a, 0x04, 0x01, 0x02, 0xac, 0x02, /* f1 = [1, 2, 300] */
        0x12, 0x03, 0x01, 0x03, 0x05, /* f2 = [-1, -2, -3] (zigzag) */
        0x1a, 0x03, 0x01, 0x00, 0x01, /* f3 = [true, false, true] */
        0x22, 0x03, 0x64, 0xc8, 0x01, /* f4 = [100, 200] */
    };

    std::vector<int32_t> i32s;
    std::vector<int32_t> s32s;
    std::vector<bool> bools;
    std::vector<uint64_t> u64s;

    ppb::reader<S> r(wire, sizeof(wire));

    CHECK(r.parse([](const ppb::reader<S> &) -> ppb_error { return PPB_OK; }, {},
              ppb::on<1>(
                  [&](auto view) -> ppb_error
                  {
                      for (auto v : view)
                      {
                          i32s.push_back(v);
                      }

                      return PPB_OK;
                  }),
              ppb::on<2>(
                  [&](auto view) -> ppb_error
                  {
                      for (auto v : view)
                      {
                          s32s.push_back(v);
                      }

                      return PPB_OK;
                  }),
              ppb::on<3>(
                  [&](auto view) -> ppb_error
                  {
                      for (auto v : view)
                      {
                          bools.push_back(v);
                      }

                      return PPB_OK;
                  }),
              ppb::on<4>(
                  [&](auto view) -> ppb_error
                  {
                      for (auto v : view)
                      {
                          u64s.push_back(v);
                      }

                      return PPB_OK;
                  })) == PPB_OK);

    CHECK(i32s.size() == 3);
    CHECK(i32s[0] == 1);
    CHECK(i32s[1] == 2);
    CHECK(i32s[2] == 300);

    CHECK(s32s.size() == 3);
    CHECK(s32s[0] == -1);
    CHECK(s32s[1] == -2);
    CHECK(s32s[2] == -3);

    CHECK(bools.size() == 3);
    CHECK(bools[0] == true);
    CHECK(bools[1] == false);
    CHECK(bools[2] == true);

    CHECK(u64s.size() == 2);
    CHECK(u64s[0] == 100);
    CHECK(u64s[1] == 200);
}

// Whenever ppb calls a handler with a view, that view must point into
// the input span, not some temporary.  Test that with ASan.
static void
test_borrowed_views_alias_input_exact()
{
    using S = ppb::schema<ppb::utf8string<1>, ppb::bytes<2>, ppb::packed_fixed32<3>, ppb::packed_int32<4>>;

    static const uint8_t wire[] = {
        0x0a, 0x05, 0x68, 0x65, 0x6c, 0x6c, 0x6f, /* f1 utf8string "hello" */
        0x12, 0x03, 0xca, 0xfe, 0xbe, /* f2 bytes {ca fe be} */
        0x1a, 0x08, 0xef, 0xbe, 0xad, 0xde, 0xbe, 0xba, 0xfe, 0xca, /* f3 packed_fixed32 */
        0x22, 0x04, 0x01, 0x02, 0xac, 0x02, /* f4 packed_int32 [1,2,300] */
    };

    const size_t n = sizeof(wire);
    auto *p = new uint8_t[n];
    std::memcpy(p, wire, n);

    auto in_input = [&](const void *q) -> bool
    {
        auto a = reinterpret_cast<uintptr_t>(q);
        return a >= reinterpret_cast<uintptr_t>(p) && a < reinterpret_cast<uintptr_t>(p) + n;
    };

    std::string_view sv;
    std::span<const std::byte> bytes_span;
    std::span<const ppb::le_packed<uint32_t>> fixed_span;
    std::optional<ppb::detail::packed_varint_view<int32_t, ppb::detail::identity_decode>> pv;

    {
        ppb::reader<S> r(p, n);
        ppb_error err = r.parse([](const ppb::reader<S> &) -> ppb_error { return PPB_OK; }, {},
            ppb::on<1>(
                [&](std::string_view s) -> ppb_error
                {
                    sv = s;
                    return PPB_OK;
                }),
            ppb::on<2>(
                [&](std::span<const std::byte> s) -> ppb_error
                {
                    bytes_span = s;
                    return PPB_OK;
                }),
            ppb::on<3>(
                [&](std::span<const ppb::le_packed<uint32_t>> s) -> ppb_error
                {
                    fixed_span = s;
                    return PPB_OK;
                }),
            ppb::on<4>(
                [&](auto view) -> ppb_error
                {
                    pv = view;
                    return PPB_OK;
                }));

        CHECK(err == PPB_OK);
        CHECK(r.empty());
    }

    /* Consumed after reader destruction: each view still reads the input bytes. */
    CHECK(sv == "hello");
    CHECK(in_input(sv.data()));

    CHECK(bytes_span.size() == 3);
    CHECK(static_cast<uint8_t>(bytes_span[0]) == 0xca);
    CHECK(in_input(bytes_span.data()));

    CHECK(fixed_span.size() == 2);
    CHECK(fixed_span[0].value() == 0xDEADBEEFu);
    CHECK(fixed_span[1].value() == 0xCAFEBABEu);
    CHECK(in_input(fixed_span.data()));

    std::vector<int32_t> pvs;
    for (auto v : *pv)
    {
        pvs.push_back(v);
    }

    CHECK(pvs.size() == 3);
    CHECK(pvs[0] == 1);
    CHECK(pvs[1] == 2);
    CHECK(pvs[2] == 300);

    delete[] p;
}

// Helpers for sticky-error + packed payload tests.
//
// check_corrupt_after_sticky: schema has error-semantics field 1
// then a packed-varint field 2.  The wire carries field 1 (triggers
// CORRUPT_TAG) and a 3-byte packed payload whose final varint is
// truncated.  The packed handler must fire (schema order dispatch),
// iterating two valid values before hitting the corrupt varint, but
// the sticky CORRUPT_TAG prevents TRUNCATED_DATA from overwriting it.
template <typename PackedField>
static void
check_corrupt_after_sticky()
{
    using S = ppb::schema<ppb::varint<1, ppb::field_semantics::error>, PackedField>;

    static const uint8_t wire[] = {
        0x08, 0x01, /* field 1 varint 1 */
        0x12, 0x03, 0x01, 0x02, 0x80, /* field 2 LEN 3 bytes, truncated varint at end */
    };

    ppb::reader<S> r(wire, sizeof(wire));

    size_t values_seen = 0;

    (void)r.prescan({}, ppb::on<1>([](const ppb_field &) -> ppb_error { return PPB_ERROR_CORRUPT_TAG; }),
        ppb::on<2>(
            [&](auto view) -> ppb_error
            {
                for (auto v : view)
                {
                    (void)v;
                    values_seen++;
                }
                return PPB_OK;
            }));

    CHECK(values_seen == 2);
    CHECK(r.error() == PPB_ERROR_CORRUPT_TAG);
}

// check_empty_payload: packed-varint field 1 with a 0-byte LEN
// payload.  Handler fires, iterates zero elements, no error.
template <typename PackedField>
static void
check_empty_payload()
{
    using S = ppb::schema<PackedField>;

    static const uint8_t wire[] = {
        0x0a, 0x00, /* f1 LEN 0 bytes */
    };

    ppb::reader<S> r(wire, sizeof(wire));

    size_t values_seen = 999;

    CHECK(r.parse([](const ppb::reader<S> &) -> ppb_error { return PPB_OK; }, {},
              ppb::on<1>(
                  [&](auto view) -> ppb_error
                  {
                      values_seen = 0;
                      for (auto v : view)
                      {
                          (void)v;
                          values_seen++;
                      }
                      return PPB_OK;
                  })) == PPB_OK);
    CHECK(values_seen == 0);
    CHECK(r.error() == PPB_OK);
}

static void
test_packed_varint_iter_default_constructed()
{
    using iter = ppb::detail::packed_varint_iter<int32_t, ppb::detail::identity_decode>;

    iter it;
    // Default-constructed: dereference returns zero-initialized T.
    CHECK(*it == 0);

    // Advancing: m_pos == m_next == nullptr, so m_pos < m_end is
    // false and decode() is skipped.  Must not crash.
    ++it;
    CHECK(*it == 0);

    // Post-increment: saves current state, pre-increments (no-op),
    // returns copy of saved state.
    iter it2 = it++;
    CHECK(*it == 0);
    CHECK(*it2 == 0);

    // Two default-constructed iterators compare equal (both m_pos == nullptr).
    iter it3;
    CHECK(it == it3);
    CHECK(!(it != it3));

    // end() from a default-constructed iterator also produces m_pos == nullptr.
    CHECK(it == it.end());
}

static void
test_merge_meta()
{
    // acc empty, upd present: full copy.
    {
        ppb_field_meta acc {};
        ppb_field_meta upd { .lost_distinct_u64 = 0,
            .num_occurrences = 1,
            .total_bytes = 5,
            .min_nonzero_bytes = 3,
            .max_bytes = 5 };
        ppb::detail::merge_meta(acc, upd);
        CHECK(acc.num_occurrences == 1);
        CHECK(acc.lost_distinct_u64 == 0);
        CHECK(acc.total_bytes == 5);
        CHECK(acc.min_nonzero_bytes == 3);
        CHECK(acc.max_bytes == 5);
    }

    // acc present, upd empty: no-op.
    {
        ppb_field_meta acc { .lost_distinct_u64 = 0,
            .num_occurrences = 2,
            .total_bytes = 10,
            .min_nonzero_bytes = 4,
            .max_bytes = 6 };
        ppb_field_meta upd {};
        ppb::detail::merge_meta(acc, upd);
        CHECK(acc.num_occurrences == 2);
        CHECK(acc.lost_distinct_u64 == 0);
        CHECK(acc.total_bytes == 10);
        CHECK(acc.min_nonzero_bytes == 4);
        CHECK(acc.max_bytes == 6);
    }

    // Both present: sums, lost_distinct_u64 forced true, min is min,
    // max is max.
    {
        ppb_field_meta acc { .lost_distinct_u64 = 0,
            .num_occurrences = 3,
            .total_bytes = 12,
            .min_nonzero_bytes = 4,
            .max_bytes = 8 };
        ppb_field_meta upd { .lost_distinct_u64 = 0,
            .num_occurrences = 2,
            .total_bytes = 7,
            .min_nonzero_bytes = 3,
            .max_bytes = 7 };
        ppb::detail::merge_meta(acc, upd);
        CHECK(acc.num_occurrences == 5);
        CHECK(acc.lost_distinct_u64 == 1);
        CHECK(acc.total_bytes == 19);
        CHECK(acc.min_nonzero_bytes == 3);
        CHECK(acc.max_bytes == 8);
    }

    // Both present, acc.min_nonzero_bytes == 0: takes upd's value.
    {
        ppb_field_meta acc { .lost_distinct_u64 = 0,
            .num_occurrences = 1,
            .total_bytes = 3,
            .min_nonzero_bytes = 0,
            .max_bytes = 3 };
        ppb_field_meta upd { .lost_distinct_u64 = 0,
            .num_occurrences = 1,
            .total_bytes = 4,
            .min_nonzero_bytes = 4,
            .max_bytes = 4 };
        ppb::detail::merge_meta(acc, upd);
        CHECK(acc.min_nonzero_bytes == 4);
    }

    // Both present, upd.min_nonzero_bytes == 0: keeps acc's value.
    {
        ppb_field_meta acc { .lost_distinct_u64 = 0,
            .num_occurrences = 1,
            .total_bytes = 5,
            .min_nonzero_bytes = 5,
            .max_bytes = 5 };
        ppb_field_meta upd { .lost_distinct_u64 = 0,
            .num_occurrences = 1,
            .total_bytes = 2,
            .min_nonzero_bytes = 0,
            .max_bytes = 2 };
        ppb::detail::merge_meta(acc, upd);
        CHECK(acc.min_nonzero_bytes == 5);
    }

    // Both present, both min_nonzero_bytes == 0: stays 0.
    {
        ppb_field_meta acc { .lost_distinct_u64 = 0,
            .num_occurrences = 1,
            .total_bytes = 3,
            .min_nonzero_bytes = 0,
            .max_bytes = 3 };
        ppb_field_meta upd { .lost_distinct_u64 = 0,
            .num_occurrences = 1,
            .total_bytes = 4,
            .min_nonzero_bytes = 0,
            .max_bytes = 4 };
        ppb::detail::merge_meta(acc, upd);
        CHECK(acc.min_nonzero_bytes == 0);
    }

    // Both present, equal min_nonzero_bytes: keeps the value.
    {
        ppb_field_meta acc { .lost_distinct_u64 = 0,
            .num_occurrences = 1,
            .total_bytes = 3,
            .min_nonzero_bytes = 3,
            .max_bytes = 3 };
        ppb_field_meta upd { .lost_distinct_u64 = 0,
            .num_occurrences = 1,
            .total_bytes = 4,
            .min_nonzero_bytes = 3,
            .max_bytes = 4 };
        ppb::detail::merge_meta(acc, upd);
        CHECK(acc.min_nonzero_bytes == 3);
    }
}

static void
test_typed_packed_enumerated_field()
{
    using S = ppb::schema<ppb::packed_enumerated<1, Color>>;

    static const uint8_t wire[] = {
        0x0a, 0x03, 0x01, 0x02, 0x03, /* f1 = [red, green, blue] */
    };

    std::vector<Color> got;
    ppb::reader<S> r(wire, sizeof(wire));

    CHECK(r.parse([](const ppb::reader<S> &) -> ppb_error { return PPB_OK; }, {},
              ppb::on<1>(
                  [&](auto view) -> ppb_error
                  {
                      for (auto c : view)
                      {
                          got.push_back(c);
                      }

                      return PPB_OK;
                  })) == PPB_OK);
    CHECK(got.size() == 3);
    CHECK(got[0] == Color::red);
    CHECK(got[1] == Color::green);
    CHECK(got[2] == Color::blue);
}

static void
test_typed_packed_varint_truncated_payload()
{
    using S = ppb::schema<ppb::packed_int32<1>>;

    /*
     * Length says 3 bytes, but the third byte has the continuation
     * bit set with no follow-up -- the iterator should set
     * TRUNCATED_DATA.
     */
    static const uint8_t wire[] = {
        0x0a, 0x03, 0x01, 0x02, 0x80, /* f1 packed_int32 = [1, 2, <truncated>] */
    };

    int values_seen = 0;
    ppb::reader<S> r(wire, sizeof(wire));
    ppb_error rc = r.parse([](const ppb::reader<S> &) -> ppb_error { return PPB_OK; }, {},
        ppb::on<1>(
            [&](auto view) -> ppb_error
            {
                for (auto v : view)
                {
                    (void)v;
                    values_seen++;
                }
                return PPB_OK;
            }));

    CHECK(rc == PPB_ERROR_TRUNCATED_DATA);
    /*
     * The iterator decodes the first two valid values before hitting the
     * truncated third varint.
     */
    CHECK(values_seen == 2);
    CHECK(r.error() == PPB_ERROR_TRUNCATED_DATA);
}

static void
test_unpacked_aliases_compile_and_decode()
{
    /*
     * unpacked_int32 aliases int32 with repeated semantics; verify that
     * dispatching via on<>() reaches the typed handler for each occurrence.
     */
    using S = ppb::schema<ppb::unpacked_int32<1>>;

    static const uint8_t wire[] = {
        0x08, 0x01, /* f1 = 1 */
        0x08, 0x02, /* f1 = 2 */
        0x08, 0x03, /* f1 = 3 */
    };

    std::vector<int32_t> got;
    ppb::reader<S> r(wire, sizeof(wire));

    CHECK(r.parse([](const ppb::reader<S> &) -> ppb_error { return PPB_OK; }, {},
              ppb::on<1>(
                  [&](int32_t v) -> ppb_error
                  {
                      got.push_back(v);
                      return PPB_OK;
                  })) == PPB_OK);

    CHECK(got.size() == 3);
    CHECK(got[0] == 1);
    CHECK(got[1] == 2);
    CHECK(got[2] == 3);
}

// Smoke test for reader getters, limit getters, and schema::num_fields().
static void
test_reader_getters()
{
    ppb::reader<TestSchema> r(four_field_wire, sizeof(four_field_wire));

    CHECK(r.error() == PPB_OK);
    CHECK(r.input().data() != nullptr);
    CHECK(r.size() == sizeof(four_field_wire));
    CHECK(!r.empty());
    CHECK(!r.error_field().has_value());
    CHECK(!r.unknown_field().has_value());

    ppb::reader<TestSchema> r2;
    CHECK(r2.size() == 0);
    CHECK(r2.empty());
    CHECK(!r2.error_field().has_value());
    CHECK(!r2.unknown_field().has_value());

    ppb::limit lim = ppb::limit::hard(100, 10);
    CHECK(lim.bytes() == 100);
    CHECK(lim.fields() == 10);
    CHECK(lim.error_on_bytes() == PPB_ERROR_LIMIT_EXCEEDED);

    // num_fields() at runtime
    CHECK(TestSchema::num_fields() == 4);
}

// Corrupt the message buffer in init (between prescan and lexn).
// Repeated semantics forces the lexn path; we just want to confirm
// we don't crash walking a buffer that mutated underneath us.
static void
test_parse_corrupt_underlying_in_init()
{
    using S = ppb::schema<ppb::varint<1, ppb::field_semantics::repeated>>;

    // Mutable stack buffer — init will overwrite it after prescan.
    uint8_t wire[] = {
        0x08, 0x01, /* field 1 varint 1 */
        0x08, 0x02, /* field 1 varint 2 */
    };

    ppb::reader<S> r(reinterpret_cast<const std::byte *>(wire), sizeof(wire));

    int handler_calls = 0;

    ppb_error rc = r.parse(
        [&wire](const ppb::reader<S> &) -> ppb_error
        {
            memset(wire, 0xFF, sizeof(wire));
            return PPB_OK;
        },
        {},
        ppb::on<1>(
            [&](const ppb_field &) -> ppb_error
            {
                handler_calls++;
                return PPB_OK;
            }));

    // Lexn should detect the corrupted buffer and set an error.
    CHECK(int(rc) < 0);
    CHECK(r.error() != PPB_OK);
    (void)handler_calls;
}

static void
test_dispatch_when_ok_and_when_error()
{
    using S = ppb::schema<ppb::varint<1>>;
    static const uint8_t wire[] = { 0x08, 0x01 }; /* field 1 varint 1 */

    ppb::reader<S> r(wire, sizeof(wire));
    CHECK(r.prescan() > 0);
    CHECK(r.error() == PPB_OK);

    // First dispatch: handler fires, returns an error to set sticky state.
    int first_calls = 0;
    CHECK(r.dispatch(ppb::on<1>(
              [&](const ppb_field &) -> ppb_error
              {
                  first_calls++;
                  return PPB_ERROR_CORRUPT_TAG;
              })) == PPB_ERROR_CORRUPT_TAG);
    CHECK(first_calls == 1);
    CHECK(r.error() == PPB_ERROR_CORRUPT_TAG);

    // Second dispatch: reader already in error state, exits early.
    int second_calls = 0;
    CHECK(r.dispatch(ppb::on<1>(
              [&](const ppb_field &) -> ppb_error
              {
                  second_calls++;
                  return PPB_OK;
              })) == PPB_ERROR_CORRUPT_TAG);
    CHECK(second_calls == 0);
    CHECK(r.error() == PPB_ERROR_CORRUPT_TAG);
}

// Reuse a reader for two parses without reset_fields() between them.
// The first parse is capped by max_fields; the second hits an unknown
// field.  Must not crash, and unknown_field() must be set after the
// second parse.
static void
test_reuse_reader_without_reset_fields()
{
    using S = ppb::auto_schema<ppb::varint<1>, ppb::detect_unknown_fields<>>;

    // Two concatenated single-field messages: field 1 varint, then
    // field 7 varint (unknown — not in the schema).
    static const uint8_t two_messages[] = {
        0x08, 0x96, 0x01, /* message 1: field 1 varint 150 */
        0x38, 0x01, /* message 2: field 7 varint 1 */
    };

    ppb::reader<S> r(two_messages, sizeof(two_messages));

    // First parse: cap at one field.
    CHECK(r.parse([](const ppb::reader<S> &) -> ppb_error { return PPB_OK; }, ppb::limit::max_fields(1)) ==
        PPB_OK);
    CHECK(r.error() == PPB_OK);
    CHECK(!r.unknown_field().has_value());

    // Second parse without reset_fields(): picks up the tail.
    CHECK(r.parse([](const ppb::reader<S> &) -> ppb_error { return PPB_OK; }) == PPB_OK);
    CHECK(r.error() == PPB_OK);
    CHECK(r.unknown_field().has_value());
}

// parse() on empty input (prescan reads 0 bytes): a valid empty message
// still runs `init`, but no field handlers fire.
static void
test_parse_empty_prescan()
{
    int init_calls = 0;

    ppb::reader<TestSchema> r(std::span<const std::byte> {});
    CHECK(r.parse(
              [&](const ppb::reader<TestSchema> &) -> ppb_error
              {
                  init_calls++;
                  return PPB_OK;
              }) == PPB_OK);
    CHECK(init_calls == 1);
    CHECK(r.empty());
}

// Empty input where `init` reports an error: it becomes the sticky error.
static void
test_parse_empty_init_error()
{
    ppb::reader<TestSchema> r(std::span<const std::byte> {});
    CHECK(r.parse([](const ppb::reader<TestSchema> &) -> ppb_error { return PPB_ERROR_CORRUPT_TAG; }) ==
        PPB_ERROR_CORRUPT_TAG);
    CHECK(r.error() == PPB_ERROR_CORRUPT_TAG);
}

// Empty input with bare handlers (no `init`): nothing fires, call succeeds.
static void
test_parse_empty_no_init()
{
    int handler_calls = 0;

    ppb::reader<TestSchema> r(std::span<const std::byte> {});
    CHECK(r.parse(ppb::on<1>(
              [&](const ppb_field &) -> ppb_error
              {
                  handler_calls++;
                  return PPB_OK;
              })) == PPB_OK);
    CHECK(handler_calls == 0);
    CHECK(r.empty());
}

// Schema with 16 fields exercises dispatch_16 cases 7-15.
using WideSchema = ppb::schema<ppb::varint<1>, ppb::varint<2>, ppb::varint<3>, ppb::varint<4>, ppb::varint<5>,
    ppb::varint<6>, ppb::varint<7>, ppb::varint<8>, ppb::varint<9>, ppb::varint<10>, ppb::varint<11>,
    ppb::varint<12>, ppb::varint<13>, ppb::varint<14>, ppb::varint<15>, ppb::varint<16>>;

static const uint8_t wide_wire[] = {
    0x08,
    0x01,
    0x10,
    0x01,
    0x18,
    0x01,
    0x20,
    0x01,
    0x28,
    0x01,
    0x30,
    0x01,
    0x38,
    0x01,
    0x40,
    0x01,
    0x48,
    0x01,
    0x50,
    0x01,
    0x58,
    0x01,
    0x60,
    0x01,
    0x68,
    0x01,
    0x70,
    0x01,
    0x78,
    0x01,
    0x80,
    0x01,
    0x01,
};

// dispatch() runtime tests: the compile-time-bounded dispatch
// helper is exercised at runtime to cover block boundaries and
// limits that are / are-not multiples of 16.  Each test loops
// over all end bounds up to the hardcoded maximum to exercise
// every possible cut point within and across dispatch_16 blocks.

static void
test_dispatch_limit_multiple_of_16()
{
    for (size_t end = 0; end <= 16; end++)
    {
        for (size_t begin = 0; begin <= end + 1; begin++)
        {
            size_t n = 0;

            ppb::detail::dispatch<16>(begin, end,
                [&]<size_t I>(std::integral_constant<size_t, I>)
                {
                    CHECK(I == begin + n);
                    n++;
                    return true;
                });
            CHECK(n == (begin < end ? end - begin : 0));
        }
    }
}

static void
test_dispatch_limit_non_multiple_of_16()
{
    for (size_t end = 0; end <= 20; end++)
    {
        for (size_t begin = 0; begin <= end + 1; begin++)
        {
            size_t n = 0;

            ppb::detail::dispatch<20>(begin, end,
                [&]<size_t I>(std::integral_constant<size_t, I>)
                {
                    CHECK(I == begin + n);
                    n++;
                    return true;
                });
            CHECK(n == (begin < end ? end - begin : 0));
        }
    }
}

static void
test_dispatch_range_2_40_of_48()
{
    /*
     * limit=48 (3 blocks of 16).
     * Block 0 visits 2..15, block 1 visits 16..31, block 2 visits 32..39.
     */
    for (size_t end = 2; end <= 40; end++)
    {
        for (size_t begin = 2; begin <= end + 1; begin++)
        {
            size_t n = 0;

            ppb::detail::dispatch<48>(begin, end,
                [&]<size_t I>(std::integral_constant<size_t, I>)
                {
                    CHECK(I == begin + n);
                    n++;
                    return true;
                });
            CHECK(n == (begin < end ? end - begin : 0));
        }
    }
}

static void
test_dispatch_early_exit()
{
    for (size_t stop_after = 0; stop_after < 20; stop_after++)
    {
        for (size_t begin = 0; begin <= stop_after + 1; begin++)
        {
            size_t n = 0;

            ppb::detail::dispatch<20>(begin, 20,
                [&]<size_t I>(std::integral_constant<size_t, I>)
                {
                    CHECK(I == begin + n);
                    n++;
                    return n <= stop_after;
                });
            size_t limited = begin < 20 ? 20 - begin : 0;
            size_t expected = stop_after + 1 < limited ? stop_after + 1 : limited;
            CHECK(n == expected);
        }
    }
}

static void
test_dispatch_limit_0()
{
    // limit=0: num_blocks=0, fold is a no-op.  No handler runs
    // regardless of begin/end values.
    struct Case
    {
        size_t begin;
        size_t end;
    };

    for (auto [begin, end] : { Case { 0, 0 }, Case { 0, 1 }, Case { 10, 5 }, Case { 5, 10 } })
    {
        size_t n = 0;

        ppb::detail::dispatch<0>(begin, end,
            [&n]<size_t I>(std::integral_constant<size_t, I>)
            {
                n++;
                return true;
            });
        CHECK(n == 0);
    }
}

static void
test_dispatch_limit_1()
{
    struct Case
    {
        size_t begin;
        size_t end;
        size_t expected;
    };

    for (auto [begin, end, expected] : {
             Case { 0, 0, 0 },
             Case { 0, 1, 1 },
             Case { 1, 0, 0 },
             Case { 1, 1, 0 },
             Case { 5, 3, 0 },
             Case { 5, 10, 0 },
             Case { 0, 1000, 1 },
         })
    {
        size_t n = 0;

        ppb::detail::dispatch<1>(begin, end,
            [&n]<size_t I>(std::integral_constant<size_t, I>)
            {
                CHECK(I == 0);
                n++;
                return true;
            });
        CHECK(n == expected);
    }
}

static void
test_dispatch_high_indices()
{
    ppb::reader<WideSchema> r(wide_wire, sizeof(wide_wire));
    CHECK(r.prescan() > 0);
    CHECK(r.error() == PPB_OK);
}

/*
 * A 20-field schema with mixed wire types, to exercise the ppb.hpp
 * dispatch logic on fields that straddle the 15/16 handler-block
 * boundary.
 */
using WideMixSchema =
    ppb::schema<ppb::varint<1>, ppb::i64<2>, ppb::len<3>, ppb::i32<4>, ppb::varint<5>, ppb::i64<6>,
        ppb::len<7>, ppb::i32<8>, ppb::varint<9>, ppb::i64<10>, ppb::len<11>, ppb::i32<12>, ppb::varint<13>,
        ppb::i64<14>, ppb::len<15>, ppb::i32<16>, ppb::varint<17>, ppb::i64<18>, ppb::len<19>, ppb::i32<20>>;

struct WideMixState
{
    bool fired[21] = {};
    uint64_t tag[21] = {};
};

// Wire code the schema declares for field K: cycle VARINT/I64/LEN/I32.
static uint64_t
wide_mix_wire_for(int K)
{
    switch ((K - 1) % 4)
    {
    case 0:
        return 0; /* VARINT */
    case 1:
        return 1; /* I64 */
    case 2:
        return 2; /* LEN */
    default:
        return 5; /* I32 */
    }
}

template <int K>
static void
wide_mix_record(WideMixState &st, const ppb_field &f, const std::byte *msg_end)
{
    st.fired[K] = true;

    const auto *tp = static_cast<const std::byte *>(f.v.ptr);
    CHECK(tp != nullptr);
    CHECK(tp < msg_end);

    /* Re-decode the tag, bounded by the message end (no over-read). */
    ppb_buf b = { .buf = tp, .size = size_t(msg_end - tp) };
    ppb_error e = PPB_OK;
    st.tag[K] = ppb_decode_varint(&b, &e);
    CHECK(e == PPB_OK);
}

template <auto K>
static auto
wide_mix_handler(WideMixState &st, const std::byte *msg_end)
{
    return ppb::on<K>(
        [&st, msg_end](const ppb_field &f) -> ppb_error
        {
            wide_mix_record<static_cast<int>(K)>(st, f, msg_end);
            return PPB_OK;
        });
}

static void
test_dispatch_wide_mixed_wire_boundary()
{
    /*
     * Fields 14..18 straddle the index-15/16 seam; payload content is
     * irrelevant (handlers read the tag, not the value).
     */
    static const uint8_t bytes[] = {
        0x71, 0x0E, 0, 0, 0, 0, 0, 0, 0, /* f14 I64 */
        0x7A, 0x02, 0xAA, 0xBB, /* f15 LEN */
        0x85, 0x01, 0x10, 0, 0, 0, /* f16 I32 (2-byte tag) */
        0x88, 0x01, 0x2A, /* f17 VARINT (2-byte tag) */
        0x91, 0x01, 0x12, 0, 0, 0, 0, 0, 0, 0, /* f18 I64 (2-byte tag) */
    };

    /* Exact-sized heap copy for ASan. */
    const size_t n = sizeof(bytes);
    auto *msg = new uint8_t[n];
    std::memcpy(msg, bytes, n);
    const std::byte *const msg_end = reinterpret_cast<const std::byte *>(msg) + n;

    auto verify = [&](WideMixState &st)
    {
        for (int K : { 14, 15, 16, 17, 18 })
        {
            CHECK(st.fired[K]);
            CHECK((st.tag[K] >> 3) == uint64_t(K)); /* dispatched index == field number */
            CHECK((st.tag[K] & 7) == wide_mix_wire_for(K)); /* and the right wire type */
        }

        CHECK(!st.fired[13]); /* absent straddling field: range check excludes it */
        CHECK(!st.fired[19]);
    };

    /* parse() fast path. */
    {
        WideMixState st;
        ppb::reader<WideMixSchema> r(msg, n);
        (void)r.parse(wide_mix_handler<13>(st, msg_end), wide_mix_handler<14>(st, msg_end),
            wide_mix_handler<15>(st, msg_end), wide_mix_handler<16>(st, msg_end),
            wide_mix_handler<17>(st, msg_end), wide_mix_handler<18>(st, msg_end),
            wide_mix_handler<19>(st, msg_end));
        CHECK(r.error() == PPB_OK);
        verify(st);
    }

    /* prescan + dispatch(): lower_bound = 1, range = UINTPTR_MAX-1. */
    {
        WideMixState st;
        ppb::reader<WideMixSchema> r(msg, n);
        CHECK(r.prescan() > 0);
        CHECK(r.dispatch(wide_mix_handler<13>(st, msg_end), wide_mix_handler<14>(st, msg_end),
                  wide_mix_handler<15>(st, msg_end), wide_mix_handler<16>(st, msg_end),
                  wide_mix_handler<17>(st, msg_end), wide_mix_handler<18>(st, msg_end),
                  wide_mix_handler<19>(st, msg_end)) == PPB_OK);
        verify(st);
    }

    /*
     * lex_all(): lexn_tuple per batch, lower_bound = base.  Default limit (one
     * batch) and forced single-field batches (advancing base across the seam).
     */
    for (ppb::limit lim : { ppb::limit {}, ppb::limit::max_fields(1) })
    {
        WideMixState st;
        ppb::reader<WideMixSchema> r(msg, n);
        CHECK(r.lex_all(lim, wide_mix_handler<13>(st, msg_end), wide_mix_handler<14>(st, msg_end),
                  wide_mix_handler<15>(st, msg_end), wide_mix_handler<16>(st, msg_end),
                  wide_mix_handler<17>(st, msg_end), wide_mix_handler<18>(st, msg_end),
                  wide_mix_handler<19>(st, msg_end)) == PPB_OK);
        verify(st);
    }

    delete[] msg;
}

// A corrupt/inconsistent schema could hand `dispatch` a begin/end
// past `num_fields`; the compile-time `if constexpr (base + I <
// limit)` guard must keep every invoked index < limit regardless.
template <size_t N>
static void
check_dispatch_out_of_range()
{
    const size_t pts[] = { 0, 1, N / 2, N - 1, N, N + 1, 2 * N + 1, SIZE_MAX };

    for (size_t begin : pts)
    {
        for (size_t end : pts)
        {
            size_t n = 0;

            ppb::detail::dispatch<N>(begin, end,
                [&]<size_t I>(std::integral_constant<size_t, I>)
                {
                    static_assert(I < N, "dispatch invoked an out-of-bounds index");
                    CHECK(I == begin + n);
                    n++;
                    return true;
                });

            size_t hi = end < N ? end : N;
            CHECK(n == (begin < hi ? hi - begin : 0));
        }
    }
}

static void
test_dispatch_out_of_range_bounds()
{
    check_dispatch_out_of_range<2>();
    check_dispatch_out_of_range<3>();
    check_dispatch_out_of_range<7>();
    check_dispatch_out_of_range<16>();
    check_dispatch_out_of_range<17>();
    check_dispatch_out_of_range<32>();
    check_dispatch_out_of_range<48>();
}

// A field number appearing under a wire type other than its schema descriptor's
// must never hand a typed handler a mismatched representation: find_tag matches
// the exact .bits (field # AND wire type), so the C core only fills a LEN slot
// from a LEN-wire match, and `payload` is a separate struct member (never
// overlaid on the scalar bytes).
//
// Parse field 2 as every wire type and assert the LEN handler only
// ever sees an input-bounded payload, and the varint slot's payload
// stays the zero-initialised {nullptr, 0}.
static void
test_wire_type_mismatch_payload_bounded()
{
    using WS = ppb::schema<ppb::varint<1>, ppb::utf8string<2>, ppb::i64<3>, ppb::i32<4>>;

    static const uint8_t f2_varint[] = { 0x10, 0xFF, 0xFF, 0x01 };  // 2:VARINT, not a match
    static const uint8_t f2_i64[] = { 0x11, 1, 2, 3, 4, 5, 6, 7, 8 };  // 2:I64, not a match
    static const uint8_t f2_i32[] = { 0x15, 0xDE, 0xAD, 0xBE, 0xEF };  // 2:I32, not a match
    static const uint8_t f2_len[] = { 0x12, 0x03, 'h', 'i', '!' };  // 2:LEN, matches
    static const uint8_t f1_len[] = { 0x0A, 0x04, 'a', 'b', 'c', 'd' };  // 1:LEN, not a match
    static const uint8_t mixed[] = { 0x08, 0x96, 0x01, 0x12, 0x02, 'x', 'y' };  // 1:VARINT + 2:LEN

    struct Msg
    {
        const uint8_t *p;
        size_t n;
    };

    const Msg msgs[] = {
        { f2_varint, sizeof(f2_varint) },
        { f2_i64, sizeof(f2_i64) },
        { f2_i32, sizeof(f2_i32) },
        { f2_len, sizeof(f2_len) },
        { f1_len, sizeof(f1_len) },
        { mixed, sizeof(mixed) },
    };

    int len_calls = 0;

    for (const Msg &m : msgs)
    {
        for (size_t cut = 0; cut <= m.n; cut++)
        {
            const char *lo = reinterpret_cast<const char *>(m.p);
            const char *hi = lo + cut;

            ppb::reader<WS> r(m.p, cut);
            (void)r.parse(ppb::on<2>(
                              [&](std::string_view sv) -> ppb_error
                              {
                                  len_calls++;
                                  CHECK(sv.empty() ||
                                      (sv.data() >= lo && sv.data() <= hi &&
                                          size_t(hi - sv.data()) >= sv.size()));
                                  return PPB_OK;
                              }),
                ppb::on<1>(
                    [&](const ppb_field &f) -> ppb_error
                    {
                        /* scalar slot: payload is a separate, zero-init member. */
                        CHECK(f.v.payload.buf == nullptr);
                        return PPB_OK;
                    }));
        }
    }

    CHECK(len_calls > 0);  // the valid-LEN inputs did reach the handler
}

// lexn sticky error: calling lexn after error is already set short-circuits.
static void
test_lexn_sticky_error()
{
    ppb::reader<TestSchema> r(truncated_tag_wire, sizeof(truncated_tag_wire));  // prescan will fail

    (void)r.prescan();
    CHECK(r.error() == PPB_ERROR_TRUNCATED_DATA);

    // lexn should short-circuit on the sticky error
    int called = 0;

    CHECK(r.lexn({},
              ppb::on<1>(
                  [&](const ppb_field &) -> ppb_error
                  {
                      called++;
                      return PPB_OK;
                  })) == PPB_ERROR_TRUNCATED_DATA);
    CHECK(called == 0);
}

// prescan sticky error: calling prescan after error is already set short-circuits.
static void
test_prescan_sticky_error()
{
    ppb::reader<TestSchema> r(truncated_tag_wire, sizeof(truncated_tag_wire));

    (void)r.prescan();
    CHECK(r.error() == PPB_ERROR_TRUNCATED_DATA);

    // second prescan short-circuits
    CHECK(r.prescan() == PPB_ERROR_TRUNCATED_DATA);
}

// parse init error branch: init returns non-OK, handlers not run.
static void
test_parse_init_error_branch()
{
    ppb::reader<TestSchema> r(four_field_wire, sizeof(four_field_wire));
    int handler_calls = 0;

    CHECK(r.parse([](const ppb::reader<TestSchema> &) -> ppb_error { return PPB_ERROR_TRUNCATED_DATA; }, {},
              ppb::on<1>(
                  [&](const ppb_field &) -> ppb_error
                  {
                      handler_calls++;
                      return PPB_OK;
                  })) == PPB_ERROR_TRUNCATED_DATA);
    CHECK(handler_calls == 0);
}

// Span with claimed size > PTRDIFF_MAX triggers TRUNCATED_DATA in constructor.
static void
test_reader_too_large_input()
{
    std::span<const std::byte> huge(static_cast<const std::byte *>(nullptr),
        size_t(std::numeric_limits<ptrdiff_t>::max()) + 1);
    ppb::reader<TestSchema> r(huge);

    CHECK(r.error() == PPB_ERROR_TRUNCATED_DATA);
}

// (const void *, size_t) constructor likewise rejects size > PTRDIFF_MAX.
static void
test_reader_too_large_input_ptr()
{
    ppb::reader<TestSchema> r(static_cast<const void *>(nullptr),
        size_t(std::numeric_limits<ptrdiff_t>::max()) + 1);

    CHECK(r.error() == PPB_ERROR_TRUNCATED_DATA);
}

/*
 * Catch-all / unknown-field detection tests.
 *
 * The wire below carries:
 *   field 1 varint 150  (known to KnownSchema)
 *   field 7 varint 1    (unknown, should hit the varint catch-all)
 *   field 9 i32 42      (unknown, should hit the i32 catch-all)
 */
static const uint8_t known_plus_unknown_wire[] = {
    0x08, 0x96, 0x01, /* field 1 varint 150 */
    0x38, 0x01, /* field 7 varint 1 (unknown) */
    0x4d, 0x2a, 0x00, 0x00, 0x00, /* field 9 i32 42 (unknown) */
};

using KnownSchema = ppb::schema<ppb::varint<1>>;
using KnownPlusUnknownSchema = ppb::auto_schema<ppb::varint<1>, ppb::detect_unknown_fields<>>;

// Without catch-alls, unknown tags are silently skipped and
// `unknown_field()` stays `nullopt`.
static void
test_unknown_field_baseline_no_catchalls()
{
    ppb::reader<KnownSchema> r(known_plus_unknown_wire, sizeof(known_plus_unknown_wire));

    CHECK(r.parse([](const ppb::reader<KnownSchema> &) -> ppb_error { return PPB_OK; }) == PPB_OK);
    CHECK(!r.unknown_field().has_value());
    CHECK(r.error() == PPB_OK);
}

static void
test_unknown_field_detects_via_parse()
{
    ppb::reader<KnownPlusUnknownSchema> r(known_plus_unknown_wire, sizeof(known_plus_unknown_wire));

    CHECK(r.parse([](const ppb::reader<KnownPlusUnknownSchema> &) -> ppb_error { return PPB_OK; }) == PPB_OK);
    CHECK(r.error() == PPB_OK);
    CHECK(r.unknown_field().has_value());
    // The wire stores the *last* unknown varint as field 7 (tag = 7*8 + 0 = 56);
    // prescan keeps the last-occurrence pointer per (catch-all) bucket, so that's
    // the tag we recover.
    CHECK(r.unknown_field() == 56);
    // Sanity: extracting field number / wire type out of the tag.
    CHECK((r.unknown_field().value() >> 3) == 7);
    CHECK((r.unknown_field().value() & 7) == uint64_t(ppb::wire_type::varint));
}

static void
test_unknown_field_detects_via_prescan()
{
    ppb::reader<KnownPlusUnknownSchema> r(known_plus_unknown_wire, sizeof(known_plus_unknown_wire));

    CHECK(r.prescan() > 0);
    CHECK(r.error() == PPB_OK);
    CHECK(r.unknown_field() == 56);
}

// Only an i32 unknown -> the i32 catch-all entry is what triggers.
static void
test_unknown_field_first_write_wins()
{
    static const uint8_t i32_only_unknown[] = {
        0x4d, 0x2a, 0x00, 0x00, 0x00, /* field 9 i32 42 (tag = 9*8 + 5 = 77) */
    };

    ppb::reader<KnownPlusUnknownSchema> r(i32_only_unknown, sizeof(i32_only_unknown));

    CHECK(r.parse([](const ppb::reader<KnownPlusUnknownSchema> &) -> ppb_error { return PPB_OK; }) == PPB_OK);
    CHECK(r.unknown_field() == 77);
}

// Sticky across `reset_fields()`, like `error_field()`.
static void
test_unknown_field_sticky_across_reset_fields()
{
    ppb::reader<KnownPlusUnknownSchema> r(known_plus_unknown_wire, sizeof(known_plus_unknown_wire));

    CHECK(r.parse([](const ppb::reader<KnownPlusUnknownSchema> &) -> ppb_error { return PPB_OK; }) == PPB_OK);
    CHECK(r.unknown_field() == 56);

    r.reset_fields();
    CHECK(r.unknown_field() == 56);
}

// Sticky across messages: after parse() sets unknown_field() from a
// first message, parsing a second message does not overwrite the value.
// We use a soft byte limit to isolate the first message from the
// concatenated buffer; the second parse() picks up the unconsumed tail.
static void
test_unknown_field_sticky_across_messages()
{
    // Two concatenated single-field messages.  First: field 7 varint
    // (tag 56).  Second: field 8 varint (tag 64).
    static const uint8_t two_messages[] = {
        0x38, 0x07, /* field 7 varint 7 */
        0x40, 0x08, /* field 8 varint 8 */
    };

    ppb::reader<KnownPlusUnknownSchema> r(two_messages, sizeof(two_messages));

    // First message only: soft limit at 2 bytes.
    CHECK(r.parse([](const ppb::reader<KnownPlusUnknownSchema> &) -> ppb_error { return PPB_OK; },
              ppb::limit::soft(2)) == PPB_OK);
    CHECK(r.unknown_field() == 56);

    // Second message from the unconsumed tail.
    CHECK(r.size() == 2);
    CHECK(r.parse([](const ppb::reader<KnownPlusUnknownSchema> &) -> ppb_error { return PPB_OK; }) == PPB_OK);
    CHECK(r.unknown_field() == 56);
}

// Known handlers still run when unknown tags are also present.
static void
test_unknown_field_known_handlers_still_run()
{
    ppb::reader<KnownPlusUnknownSchema> r(known_plus_unknown_wire, sizeof(known_plus_unknown_wire));

    int known_calls = 0;
    uint64_t known_value = 0;

    CHECK(r.parse([](const ppb::reader<KnownPlusUnknownSchema> &) -> ppb_error { return PPB_OK; }, {},
              ppb::on<1, ppb::wire_type::varint>(
                  [&](const ppb_field &f) -> ppb_error
                  {
                      known_calls++;
                      known_value = f.v.u64;
                      return PPB_OK;
                  })) == PPB_OK);
    CHECK(known_calls == 1);
    CHECK(known_value == 150);
    CHECK(r.unknown_field() == 56);
}

// `ppb::on_unknown(...)` fires for catch-all fields and receives the
// raw `ppb_field`.  Handler also coexists with `ppb::on<>(...)` for
// real fields in the same `parse()` call.
static void
test_on_unknown_dispatches_per_occurrence()
{
    // Two unknown varints (fields 7 and 8) plus the known field 1.
    static const uint8_t multi_unknown_wire[] = {
        0x08, 0x96, 0x01, /* field 1 varint 150 (known) */
        0x38, 0x07, /* field 7 varint 7 (unknown, tag = 7*8 = 56) */
        0x40, 0x08, /* field 8 varint 8 (unknown, tag = 8*8 = 64) */
    };

    ppb::reader<KnownPlusUnknownSchema> r(multi_unknown_wire, sizeof(multi_unknown_wire));

    int known_calls = 0;
    int unknown_calls = 0;
    std::vector<uint64_t> unknown_values;

    CHECK(r.parse([](const ppb::reader<KnownPlusUnknownSchema> &) -> ppb_error { return PPB_OK; }, {},
              ppb::on<1, ppb::wire_type::varint>(
                  [&](const ppb_field &) -> ppb_error
                  {
                      known_calls++;
                      return PPB_OK;
                  }),
              ppb::on_unknown(
                  [&](const ppb_field &f) -> ppb_error
                  {
                      unknown_calls++;
                      unknown_values.push_back(f.v.u64);
                      return PPB_OK;
                  })) == PPB_OK);
    CHECK(known_calls == 1);
    CHECK(unknown_calls == 2);
    CHECK(unknown_values.size() == 2);
    CHECK(unknown_values[0] == 7);
    CHECK(unknown_values[1] == 8);
}

// `on_unknown<wire>` only matches that wire type's catch-all.
static void
test_on_unknown_per_wire_dispatch()
{
    // One unknown varint (field 7) and one unknown i32 (field 9).
    static const uint8_t mixed_wire[] = {
        0x38, 0x01, /* field 7 varint 1 (unknown) */
        0x4d, 0x2a, 0x00, 0x00, 0x00, /* field 9 i32 42 (unknown) */
    };

    using S = ppb::auto_schema<ppb::varint<1>, ppb::detect_unknown_fields<>>;
    ppb::reader<S> r(mixed_wire, sizeof(mixed_wire));

    int varint_calls = 0;
    int i32_calls = 0;

    CHECK(r.parse([](const ppb::reader<S> &) -> ppb_error { return PPB_OK; }, {},
              ppb::on_unknown<ppb::wire_type::varint>(
                  [&](const ppb_field &) -> ppb_error
                  {
                      varint_calls++;
                      return PPB_OK;
                  }),
              ppb::on_unknown<ppb::wire_type::i32>(
                  [&](const ppb_field &) -> ppb_error
                  {
                      i32_calls++;
                      return PPB_OK;
                  })) == PPB_OK);
    CHECK(varint_calls == 1);
    CHECK(i32_calls == 1);
}

// `on_unknown` handler errors fold into the reader's sticky error.
static void
test_on_unknown_handler_error_propagates()
{
    ppb::reader<KnownPlusUnknownSchema> r(known_plus_unknown_wire, sizeof(known_plus_unknown_wire));

    CHECK(r.parse([](const ppb::reader<KnownPlusUnknownSchema> &) -> ppb_error { return PPB_OK; }, {},
              ppb::on_unknown([](const ppb_field &) -> ppb_error { return PPB_ERROR_TRUNCATED_DATA; })) ==
        PPB_ERROR_TRUNCATED_DATA);
    CHECK(r.error() == PPB_ERROR_TRUNCATED_DATA);
}

// proto3_uint32 alias: absent field dispatches with zero, matching the
// explicit proto3_zero_default spelling.
static void
test_proto3_uint32_parse_absent_dispatches_zero()
{
    using S = ppb::schema<ppb::proto3_uint32<1>>;
    static const uint8_t wire[] = {
        0x10, 0x01, /* field 2 varint 1 (not field 1) */
    };
    ppb::reader<S> r(wire, sizeof(wire));

    int calls = 0;
    uint32_t seen = 99;

    CHECK(r.parse([](const ppb::reader<S> &) -> ppb_error { return PPB_OK; }, {},
              ppb::on<1>(
                  [&](uint32_t v) -> ppb_error
                  {
                      calls++;
                      seen = v;
                      return PPB_OK;
                  })) == PPB_OK);
    CHECK(calls == 1);
    CHECK(seen == 0);
    CHECK(r.empty());
}

static void
test_handler_factories()
{
    enum class K : uint8_t
    {
        one = 1,
        two = 2,
    };
    // store: scalar field
    {
        using S = ppb::schema<ppb::int32<K::one>>;
        static const uint8_t wire[] = { 0x08, 0x2a }; /* f1 varint = 42 */
        int32_t v = 0;
        ppb::reader<S> r(wire, sizeof(wire));
        CHECK(r.parse(ppb::store<K::one>(&v)) == PPB_OK);
        CHECK(v == 42);
    }

    // store: string_view field
    {
        using S = ppb::schema<ppb::utf8string<K::one>>;
        static const uint8_t wire[] = {
            0x0a, 0x05, 'h', 'e', 'l', 'l', 'o', /* f1 = "hello" */
        };
        std::string_view v;
        ppb::reader<S> r(wire, sizeof(wire));
        CHECK(r.parse(ppb::store<K::one>(&v)) == PPB_OK);
        CHECK(v == std::string_view("hello"));
    }

    // store: bytes field
    {
        using S = ppb::schema<ppb::bytes<K::one>>;
        static const uint8_t wire[] = {
            0x0a, 0x03, 0xca, 0xfe, 0xbe, /* f1 bytes = {0xCA, 0xFE, 0xBE} */
        };
        std::span<const std::byte> v;
        ppb::reader<S> r(wire, sizeof(wire));
        CHECK(r.parse(ppb::store<K::one>(&v)) == PPB_OK);
        CHECK(v.size() == 3);
        CHECK(static_cast<uint8_t>(v[0]) == 0xca);
    }

    // push_back: repeated int32 (per-occurrence dispatch)
    {
        using S = ppb::schema<ppb::unpacked_int32<K::one>>;
        static const uint8_t wire[] = {
            0x08, 0x01, /* f1 = 1 */
            0x08, 0x02, /* f1 = 2 */
            0x08, 0x03, /* f1 = 3 */
        };
        std::vector<int32_t> v;
        ppb::reader<S> r(wire, sizeof(wire));
        CHECK(r.parse(ppb::push_back<K::one>(&v)) == PPB_OK);
        CHECK(v.size() == 3);
        CHECK(v[0] == 1);
        CHECK(v[1] == 2);
        CHECK(v[2] == 3);
    }

    // push_back: repeated string_view
    {
        using S = ppb::schema<ppb::unpacked_utf8string<K::one>>;
        static const uint8_t wire[] = {
            0x0a, 0x03, 'f', 'o', 'o', /* f1 = "foo" */
            0x0a, 0x03, 'b', 'a', 'r', /* f1 = "bar" */
        };
        std::vector<std::string_view> v;
        ppb::reader<S> r(wire, sizeof(wire));
        CHECK(r.parse(ppb::push_back<K::one>(&v)) == PPB_OK);
        CHECK(v.size() == 2);
        CHECK(v[0] == "foo");
        CHECK(v[1] == "bar");
    }

    // emplace_back: repeated int64
    {
        using S = ppb::schema<ppb::unpacked_int64<K::one>>;
        static const uint8_t wire[] = {
            0x08, 0x01, /* f1 = 1 */
            0x08, 0x02, /* f1 = 2 */
        };
        std::vector<int64_t> v;
        ppb::reader<S> r(wire, sizeof(wire));
        CHECK(r.parse(ppb::emplace_back<K::one>(&v)) == PPB_OK);
        CHECK(v.size() == 2);
        CHECK(v[0] == 1);
        CHECK(v[1] == 2);
    }

    // push_back: packed_fixed32 iterates element-by-element over span<const le_packed<uint32_t>>
    {
        using S = ppb::schema<ppb::packed_fixed32<K::one>>;
        static const uint8_t wire[] = {
            0x0a, 0x08, /* f1 bytes header (8 payload bytes) */
            0xef, 0xbe, 0xad, 0xde, /* 0xDEADBEEF */
            0xbe, 0xba, 0xfe, 0xca, /* 0xCAFEBABE */
        };
        std::vector<uint32_t> v;
        ppb::reader<S> r(wire, sizeof(wire));
        CHECK(r.parse(ppb::push_back<K::one>(&v)) == PPB_OK);
        CHECK(v.size() == 2);
        CHECK(v[0] == 0xDEADBEEFU);
        CHECK(v[1] == 0xCAFEBABEU);
    }

    // push_back: packed_int32 iterates element-by-element over packed_varint_view
    {
        using S = ppb::schema<ppb::packed_int32<K::one>>;
        static const uint8_t wire[] = {
            0x0a, 0x04, 0x01, 0x02, 0xac, 0x02, /* f1 = [1, 2, 300] */
        };
        std::vector<int32_t> v;
        ppb::reader<S> r(wire, sizeof(wire));
        CHECK(r.parse(ppb::push_back<K::one>(&v)) == PPB_OK);
        CHECK(v.size() == 3);
        CHECK(v[0] == 1);
        CHECK(v[1] == 2);
        CHECK(v[2] == 300);
    }

    // emplace_back: packed_fixed64 iterates element-by-element over span<const le_packed<uint64_t>>
    {
        using S = ppb::schema<ppb::packed_fixed64<K::one>>;
        static const uint8_t wire[] = {
            0x0a, 0x10, /* f1 bytes header (16 bytes) */
            0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 1 */
            0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 2 */
        };
        std::vector<uint64_t> v;
        ppb::reader<S> r(wire, sizeof(wire));
        CHECK(r.parse(ppb::emplace_back<K::one>(&v)) == PPB_OK);
        CHECK(v.size() == 2);
        CHECK(v[0] == 1);
        CHECK(v[1] == 2);
    }

    // emplace_back: packed_sint64 iterates element-by-element over packed_varint_view
    {
        using S = ppb::schema<ppb::packed_sint64<K::one>>;
        static const uint8_t wire[] = {
            0x0a, 0x03, 0x01, 0x03, 0x05, /* f1 = [-1, -2, -3] (zigzag: odds are negative) */
        };
        std::vector<int64_t> v;
        ppb::reader<S> r(wire, sizeof(wire));
        CHECK(r.parse(ppb::emplace_back<K::one>(&v)) == PPB_OK);
        CHECK(v.size() == 3);
        CHECK(v[0] == -1);
        CHECK(v[1] == -2);
        CHECK(v[2] == -3);
    }

    // parse() with two handlers: overload resolution picks Handlers... directly.
    {
        using S = ppb::schema<ppb::int32<K::one>, ppb::int32<K::two>>;
        static const uint8_t wire[] = {
            0x08, 0x2a, /* f1 = 42 */
            0x10, 0x63, /* f2 = 99 */
        };
        int32_t a = 0, b = 0;
        ppb::reader<S> r(wire, sizeof(wire));
        CHECK(r.parse(ppb::store<K::one>(&a), ppb::store<K::two>(&b)) == PPB_OK);
        CHECK(a == 42);
        CHECK(b == 99);
    }
}

// ---- on_submessage tests --------------------------------------------------

namespace submsg_tests
{

// Inner message: field 1 varint, field 2 utf8string.
using Inner = ppb::schema<ppb::varint<1>, ppb::utf8string<2>>;

// Outer message with raw bytes for the nested payload (no schema check).
using OuterRaw = ppb::schema<ppb::bytes<1>, ppb::varint<2>>;

// Outer message with `ppb::message<K, S>` for compile-time linkage.
using OuterTyped = ppb::schema<ppb::message<1, Inner>, ppb::varint<2>>;

// Wire helpers ---------------------------------------------------------------

// Encodes inner = { f1=varint:42, f2="hi" } -> 4 bytes: 08 2a 12 02 68 69
static const uint8_t inner_wire[] = { 0x08, 0x2a, 0x12, 0x02, 0x68, 0x69 };

// Outer wire: f1 = LEN(inner_wire), f2 = varint 7.
//   0x0a (tag 1, LEN), 0x06 (len), inner_wire bytes, 0x10 (tag 2, varint), 0x07
static const uint8_t outer_wire[] = {
    0x0a, 0x06, 0x08, 0x2a, 0x12, 0x02, 0x68, 0x69,  // 0x0a (tag 1, LEN), 0x06 (len), inner_wire bytes
    0x10, 0x07,  // 0x10 (tag 2, varint), 0x07
};

// Three nestings (depth-3): L1{ f1 = LEN(L2{ f1 = LEN(L3{ f1 = LEN(Leaf{ f1 = varint 42 }) }) }) }
using Leaf = ppb::schema<ppb::varint<1>>;
using L3 = ppb::schema<ppb::bytes<1>>;
using L2 = ppb::schema<ppb::bytes<1>>;
using L1 = ppb::schema<ppb::bytes<1>>;

// Leaf:  08 2a                        (f1 varint 42)               -> 2 bytes
// L3:    0a 02 08 2a                  (f1 = LEN(2) of leaf)        -> 4 bytes
// L2:    0a 04 0a 02 08 2a            (f1 = LEN(4) of L3)          -> 6 bytes
static const uint8_t deep_wire[] = {
    0x0a, 0x06, 0x0a, 0x04, 0x0a, 0x02, 0x08, 0x2a,  // f1 = LEN(6) of L2
};

static void
test_on_submessage_two_level()
{
    ppb::reader<OuterRaw> r(outer_wire, sizeof(outer_wire));

    uint64_t inner_v = 0;
    std::string_view inner_s;
    uint64_t outer_v = 0;

    auto bounds = ppb::limit::max_depth(4);
    ppb_error err = r.parse(bounds,
        ppb::on_submessage<1, Inner>(ppb::on<1>(
                                         [&](const ppb_field &f) -> ppb_error
                                         {
                                             inner_v = f.v.u64;
                                             return PPB_OK;
                                         }),
            ppb::on<2>(
                [&](std::string_view sv) -> ppb_error
                {
                    inner_s = sv;
                    return PPB_OK;
                })),
        ppb::on<2>(
            [&](const ppb_field &f) -> ppb_error
            {
                outer_v = f.v.u64;
                return PPB_OK;
            }));

    CHECK(err == PPB_OK);
    CHECK(inner_v == 42);
    CHECK(inner_s == "hi");
    CHECK(outer_v == 7);
}

static void
test_on_submessage_three_level_ok_at_depth_3()
{
    ppb::reader<L1> r(deep_wire, sizeof(deep_wire));

    uint64_t leaf_v = 0;

    ppb_error err = r.parse(ppb::limit::max_depth(3),
        ppb::on_submessage<1, L2>(ppb::on_submessage<1, L3>(ppb::on_submessage<1, Leaf>(ppb::on<1>(
            [&](const ppb_field &f) -> ppb_error
            {
                leaf_v = f.v.u64;
                return PPB_OK;
            })))));

    CHECK(err == PPB_OK);
    CHECK(leaf_v == 42);
}

static void
test_on_submessage_three_level_fails_at_depth_2()
{
    ppb::reader<L1> r(deep_wire, sizeof(deep_wire));

    bool leaf_fired = false;

    ppb_error err = r.parse(ppb::limit::max_depth(2),
        ppb::on_submessage<1, L2>(ppb::on_submessage<1, L3>(ppb::on_submessage<1, Leaf>(ppb::on<1>(
            [&](const ppb_field &) -> ppb_error
            {
                leaf_fired = true;
                return PPB_OK;
            })))));

    CHECK(err == PPB_ERROR_DEPTH_EXCEEDED);
    CHECK(r.error() == PPB_ERROR_DEPTH_EXCEEDED);
    CHECK(!leaf_fired);
}

static void
test_on_submessage_default_limit_fails_closed()
{
    ppb::reader<OuterRaw> r(outer_wire, sizeof(outer_wire));

    bool inner_fired = false;
    ppb_error err = r.parse(  // default limit{} → max_depth = 0
        ppb::on_submessage<1, Inner>(ppb::on<1>(
            [&](const ppb_field &) -> ppb_error
            {
                inner_fired = true;
                return PPB_OK;
            })));

    CHECK(err == PPB_ERROR_DEPTH_EXCEEDED);
    CHECK(r.error() == PPB_ERROR_DEPTH_EXCEEDED);
    CHECK(!inner_fired);
}

static void
test_on_submessage_inner_handler_error_propagates()
{
    ppb::reader<OuterRaw> r(outer_wire, sizeof(outer_wire));

    ppb_error err = r.parse(ppb::limit::max_depth(2),
        ppb::on_submessage<1, Inner>(
            ppb::on<1>([&](const ppb_field &) -> ppb_error { return PPB_ERROR_CORRUPT_TAG; })));

    CHECK(err == PPB_ERROR_CORRUPT_TAG);
    CHECK(r.error() == PPB_ERROR_CORRUPT_TAG);
}

// Repeated submessages: outer schema has unpacked_bytes for the nested
// field, so each occurrence dispatches to a fresh inner reader.
using OuterRepeated = ppb::schema<ppb::unpacked_bytes<1>, ppb::varint<2>>;

// Two inner instances back-to-back at field 1.
static const uint8_t outer_repeated_wire[] = {
    0x0a, 0x06, 0x08, 0x2a, 0x12, 0x02, 0x68, 0x69,  // f1 = inner{42, "hi"}
    0x0a, 0x06, 0x08, 0x63, 0x12, 0x02, 0x6e, 0x6f,  // f1 = inner{99, "no"}
    0x10, 0x07,  // f2 = 7
};

static void
test_on_submessage_repeated()
{
    ppb::reader<OuterRepeated> r(outer_repeated_wire, sizeof(outer_repeated_wire));

    std::vector<uint64_t> inner_vs;
    std::vector<std::string> inner_ss;

    ppb_error err = r.parse(ppb::limit::max_depth(2),
        ppb::on_submessage<1, Inner>(ppb::on<1>(
                                         [&](const ppb_field &f) -> ppb_error
                                         {
                                             inner_vs.push_back(f.v.u64);
                                             return PPB_OK;
                                         }),
            ppb::on<2>(
                [&](std::string_view sv) -> ppb_error
                {
                    inner_ss.emplace_back(sv);
                    return PPB_OK;
                })));

    CHECK(err == PPB_OK);
    CHECK(inner_vs.size() == 2);
    CHECK(inner_vs.size() == 2 && inner_vs[0] == 42 && inner_vs[1] == 99);
    CHECK(inner_ss.size() == 2 && inner_ss[0] == "hi" && inner_ss[1] == "no");
}

static void
test_on_submessage_with_init()
{
    ppb::reader<OuterRaw> r(outer_wire, sizeof(outer_wire));

    bool init_fired = false;
    size_t inner_field1_count = 0;
    uint64_t v = 0;

    ppb_error err = r.parse(ppb::limit::max_depth(2),
        ppb::on_submessage<1, Inner>(
            [&](const ppb::reader<Inner> &ir) -> ppb_error
            {
                init_fired = true;
                inner_field1_count = ir.template meta<1>().num_occurrences;
                return PPB_OK;
            },
            ppb::on<1>(
                [&](const ppb_field &f) -> ppb_error
                {
                    v = f.v.u64;
                    return PPB_OK;
                })));

    CHECK(err == PPB_OK);
    CHECK(init_fired);
    CHECK(inner_field1_count == 1);
    CHECK(v == 42);
}

static void
test_on_submessage_typed_message_field()
{
    // Schema declares `ppb::message<1, Inner>` so the link is checked at
    // compile time; runtime behavior identical to the raw-bytes case.
    ppb::reader<OuterTyped> r(outer_wire, sizeof(outer_wire));

    uint64_t inner_v = 0;

    ppb_error err = r.parse(ppb::limit::max_depth(2),
        ppb::on_submessage<1, Inner>(ppb::on<1>(
            [&](const ppb_field &f) -> ppb_error
            {
                inner_v = f.v.u64;
                return PPB_OK;
            })));

    CHECK(err == PPB_OK);
    CHECK(inner_v == 42);
}

// A present-but-empty submessage (LEN, length 0) must still fire `init`
// so the child message is materialised despite carrying no fields.
static const uint8_t outer_empty_submsg_wire[] = { 0x0a, 0x00 };

static void
test_on_submessage_zero_length_runs_init()
{
    ppb::reader<OuterRaw> r(outer_empty_submsg_wire, sizeof(outer_empty_submsg_wire));

    bool init_fired = false;
    size_t inner_field1_count = 99;
    int inner_handler_calls = 0;

    ppb_error err = r.parse(ppb::limit::max_depth(2),
        ppb::on_submessage<1, Inner>(
            [&](const ppb::reader<Inner> &ir) -> ppb_error
            {
                init_fired = true;
                inner_field1_count = ir.template meta<1>().num_occurrences;
                return PPB_OK;
            },
            ppb::on<1>(
                [&](const ppb_field &) -> ppb_error
                {
                    inner_handler_calls++;
                    return PPB_OK;
                })));

    CHECK(err == PPB_OK);
    CHECK(init_fired);
    CHECK(inner_field1_count == 0);
    CHECK(inner_handler_calls == 0);
}

/*
 * A singular (`ppb::message<K, S>`) field that appears more than once
 * must be descended into on EVERY occurrence, the way libprotobuf
 * parses (and merges) each one.  The divergence shows up as
 * undetected malformed messages, and incompletely parsed submessages
 * when a later entry overrides only a subset of fields.
 */
using SingInner = ppb::schema<ppb::varint<1>>;
using SingOuter = ppb::schema<ppb::message<1, SingInner>>;

static void
test_on_submessage_validates_every_occurrence()
{
    // First occurrence is malformed (LEN(1) holding tag 0x08 with its varint
    // value truncated); the second is an empty message.  Descending into every
    // occurrence rejects on the malformed first one.
    static const uint8_t malformed_then_empty[] = {
        0x0a, 0x01, 0x08,  // field 1 = LEN(1) { tag 1 varint, value truncated }
        0x0a, 0x00,  // field 1 = LEN(0) empty
    };

    {
        ppb::reader<SingOuter> r(malformed_then_empty, sizeof(malformed_then_empty));

        ppb_error err = r.parse(ppb::limit::max_depth(2),
            ppb::on_submessage<1, SingInner>(
                ppb::on<1>([](const ppb_field &) -> ppb_error { return PPB_OK; })));

        CHECK(err != PPB_OK);
    }

    // Both occurrences valid: the first carries field 1 = 1, the second is
    // empty.  Merging across occurrences keeps the value from the first.
    static const uint8_t valued_then_empty[] = {
        0x0a, 0x02, 0x08, 0x01,  // field 1 = LEN(2) { tag 1 varint, value 1 }
        0x0a, 0x00,  // field 1 = LEN(0) empty
    };

    {
        ppb::reader<SingOuter> r(valued_then_empty, sizeof(valued_then_empty));

        uint64_t inner_v = 0;
        ppb_error err = r.parse(ppb::limit::max_depth(2),
            ppb::on_submessage<1, SingInner>(ppb::on<1>(
                [&](const ppb_field &f) -> ppb_error
                {
                    inner_v = f.v.u64;
                    return PPB_OK;
                })));

        CHECK(err == PPB_OK);
        CHECK(inner_v == 1);
    }
}

}  // namespace submsg_tests

namespace repeated_tests
{

/*
 * Parse `wire` as `Schema` and collect field `Key` element-by-element via
 * on_each into a vector of T.
 */
template <typename Schema, auto Key, typename T>
static std::vector<T>
collect_on_each(std::initializer_list<uint8_t> wire)
{
    std::vector<std::byte> buf;

    for (uint8_t b : wire)
    {
        buf.push_back(std::byte { b });
    }

    std::vector<T> out;
    ppb::reader<Schema> r(std::span<const std::byte>(buf.data(), buf.size()));

    (void)r.parse(ppb::on_each<Key>([&](T v) { out.push_back(v); }));
    return out;
}

/* packed and unpacked decode identically. */
static void
test_int32_packed_equals_unpacked()
{
    enum class K : int
    {
        v = 1
    };
    using S = ppb::auto_schema<ppb::packed_int32<K::v>, ppb::int32<K::v, ppb::field_semantics::always_lexn>>;

    auto from_packed = collect_on_each<S, K::v, int32_t>({ 0x0a, 0x03, 1, 2, 3 });
    auto from_unpacked = collect_on_each<S, K::v, int32_t>({ 0x08, 1, 0x08, 2, 0x08, 3 });
    const std::vector<int32_t> expected = { 1, 2, 3 };
    CHECK(from_packed == expected);
    CHECK(from_unpacked == expected);
}

static void
test_sfixed32_packed_equals_unpacked()
{
    enum class K : int
    {
        v = 1
    };
    using S =
        ppb::auto_schema<ppb::packed_sfixed32<K::v>, ppb::sfixed32<K::v, ppb::field_semantics::always_lexn>>;

    /* little-endian: 1, 2, -1. */
    auto from_packed = collect_on_each<S, K::v, int32_t>(
        { 0x0a, 0x0c, 1, 0, 0, 0, 2, 0, 0, 0, 0xff, 0xff, 0xff, 0xff });
    auto from_unpacked = collect_on_each<S, K::v, int32_t>(
        { 0x0d, 1, 0, 0, 0, 0x0d, 2, 0, 0, 0, 0x0d, 0xff, 0xff, 0xff, 0xff });
    const std::vector<int32_t> expected = { 1, 2, -1 };
    CHECK(from_packed == expected);
    CHECK(from_unpacked == expected);
}

static void
test_double_packed_equals_unpacked()
{
    enum class K : int
    {
        v = 1
    };
    using S = ppb::auto_schema<ppb::packed_f64<K::v>, ppb::f64<K::v, ppb::field_semantics::always_lexn>>;

    /* IEEE 754 LE: 1.0=3FF0.., 2.0=4000.., 3.0=4008.. */
    auto from_packed = collect_on_each<S, K::v, double>({ 0x0a, 0x18, 0, 0, 0, 0, 0, 0, 0xf0, 0x3f, 0, 0, 0,
        0, 0, 0, 0x00, 0x40, 0, 0, 0, 0, 0, 0, 0x08, 0x40 });
    auto from_unpacked = collect_on_each<S, K::v, double>({ 0x09, 0, 0, 0, 0, 0, 0, 0xf0, 0x3f, 0x09, 0, 0, 0,
        0, 0, 0, 0x00, 0x40, 0x09, 0, 0, 0, 0, 0, 0, 0x08, 0x40 });
    const std::vector<double> expected = { 1.0, 2.0, 3.0 };
    CHECK(from_packed == expected);
    CHECK(from_unpacked == expected);
}

static void
test_enum_packed_equals_unpacked()
{
    enum class K : int
    {
        v = 1
    };
    enum class Color : int
    {
        red = 1,
        green = 2,
        blue = 3
    };
    using S = ppb::auto_schema<ppb::packed_enumerated<K::v, Color>,
        ppb::enumerated<K::v, Color, ppb::field_semantics::always_lexn>>;

    auto from_packed = collect_on_each<S, K::v, Color>({ 0x0a, 0x03, 1, 2, 3 });
    auto from_unpacked = collect_on_each<S, K::v, Color>({ 0x08, 1, 0x08, 2, 0x08, 3 });
    const std::vector<Color> expected = { Color::red, Color::green, Color::blue };
    CHECK(from_packed == expected);
    CHECK(from_unpacked == expected);
}

/*
 * a stateful handler observes every element of the field across both
 * wire forms of one message (packed [10,20] + unpacked 30 -> 3 calls,
 * sum 60).
 */
static void
test_stateful_handler_across_both_forms()
{
    enum class K : int
    {
        v = 1
    };
    using S = ppb::auto_schema<ppb::packed_int32<K::v>, ppb::int32<K::v, ppb::field_semantics::always_lexn>>;

    /* packed [10,20] then unpacked 30 in one message. */
    const uint8_t wire[] = { 0x0a, 0x02, 10, 20, 0x08, 30 };

    int call_count = 0;
    int64_t sum = 0;
    ppb::reader<S> r(std::as_bytes(std::span(wire)));
    (void)r.parse(ppb::on_each<K::v>(
        [&](int32_t x)
        {
            call_count++;
            sum += x;
        }));
    CHECK(call_count == 3);
    CHECK(sum == 60);
}

static void
test_stateful_handler_by_value()
{
    enum class K : int
    {
        v = 1
    };
    using S = ppb::auto_schema<ppb::packed_int32<K::v>, ppb::int32<K::v, ppb::field_semantics::always_lexn>>;

    const uint8_t wire[] = { 0x0a, 0x02, 10, 20, 0x08, 30 };

    /*
     * on_each keeps a single copy of the handler, so a by-value counter tallies
     * all three elements across both wire forms.  max_count_seen (by reference)
     * observes that single copy; the outer `count` stays 0.
     */
    int count = 0;
    int max_count_seen = -1;
    ppb::reader<S> r(std::as_bytes(std::span(wire)));
    (void)r.parse(ppb::on_each<K::v>(
        [count, &max_count_seen](int32_t) mutable
        {
            count++;
            if (count > max_count_seen)
            {
                max_count_seen = count;
            }
        }));

    CHECK(max_count_seen == 3);
    CHECK(count == 0);
}

/* error folding / short-circuit / stickiness. */
static void
test_error_short_circuits_packed_loop()
{
    enum class K : int
    {
        v = 1
    };
    using S = ppb::auto_schema<ppb::packed_int32<K::v>, ppb::int32<K::v, ppb::field_semantics::always_lexn>>;
    const uint8_t wire[] = { 0x0a, 0x03, 1, 2, 3 };

    std::vector<int32_t> seen;
    ppb::reader<S> r(std::as_bytes(std::span(wire)));
    ppb_error rc = r.parse(ppb::on_each<K::v>(
        [&](int32_t x) -> ppb_error
        {
            seen.push_back(x);
            return x == 2 ? PPB_ERROR_TRUNCATED_DATA : PPB_OK;
        }));
    CHECK(rc == PPB_ERROR_TRUNCATED_DATA);
    CHECK(r.error() == PPB_ERROR_TRUNCATED_DATA);
    CHECK(seen.size() == 2); /* 3 was never delivered */
    CHECK(seen[0] == 1 && seen[1] == 2);
}

static void
test_error_short_circuits_unpacked()
{
    enum class K : int
    {
        v = 1
    };
    using S = ppb::auto_schema<ppb::packed_int32<K::v>, ppb::int32<K::v, ppb::field_semantics::always_lexn>>;
    const uint8_t wire[] = { 0x08, 1, 0x08, 2, 0x08, 3 };

    std::vector<int32_t> seen;
    ppb::reader<S> r(std::as_bytes(std::span(wire)));
    ppb_error rc = r.parse(ppb::on_each<K::v>(
        [&](int32_t x) -> ppb_error
        {
            seen.push_back(x);
            return x == 2 ? PPB_ERROR_TRUNCATED_DATA : PPB_OK;
        }));
    CHECK(rc == PPB_ERROR_TRUNCATED_DATA);
    CHECK(r.error() == PPB_ERROR_TRUNCATED_DATA);
    CHECK(seen.size() == 2); /* 3 was never delivered */
    CHECK(seen[0] == 1 && seen[1] == 2);
}

static void
test_on_bulk_range_fn_error()
{
    enum class K : int
    {
        v = 1
    };
    using S = ppb::auto_schema<ppb::packed_int32<K::v>, ppb::int32<K::v, ppb::field_semantics::always_lexn>>;
    const uint8_t wire[] = { 0x0a, 0x03, 1, 2, 3 };

    ppb::reader<S> r(std::as_bytes(std::span(wire)));
    ppb_error rc = r.parse(ppb::on_bulk<K::v>([&](auto) -> ppb_error { return PPB_ERROR_CORRUPT_TAG; },
        [&](int32_t) -> ppb_error { return PPB_OK; }));
    CHECK(rc == PPB_ERROR_CORRUPT_TAG);
    CHECK(r.error() == PPB_ERROR_CORRUPT_TAG);
}

static void
test_void_on_each_yields_ok()
{
    enum class K : int
    {
        v = 1
    };
    using S = ppb::auto_schema<ppb::packed_int32<K::v>, ppb::int32<K::v, ppb::field_semantics::always_lexn>>;
    const uint8_t wire[] = { 0x0a, 0x03, 1, 2, 3 };

    int n = 0;
    ppb::reader<S> r(std::as_bytes(std::span(wire)));
    ppb_error rc = r.parse(ppb::on_each<K::v>([&](int32_t) { n++; }));
    CHECK(rc == PPB_OK);
    CHECK(r.error() == PPB_OK);
    CHECK(n == 3);
}

/* on_bulk for fixed-width field. */
static void
test_on_bulk_fixed_width()
{
    enum class K : int
    {
        w = 2
    };
    using S =
        ppb::auto_schema<ppb::packed_fixed32<K::w>, ppb::fixed32<K::w, ppb::field_semantics::always_lexn>>;
    const uint8_t packed[] = { 0x12, 0x08, 1, 0, 0, 0, 2, 0, 0, 0 };

    std::vector<uint32_t> out;
    auto h = ppb::on_bulk<K::w>(
        [&](std::span<const ppb::le_packed<uint32_t>> view) -> ppb_error
        {
            out.reserve(out.size() + view.size());
            for (auto w : view)
            {
                out.push_back(w.value());
            }

            return PPB_OK;
        },
        [&](uint32_t v) -> ppb_error
        {
            out.push_back(v);
            return PPB_OK;
        });

    ppb::reader<S> r(std::as_bytes(std::span(packed)));
    CHECK(r.parse(h) == PPB_OK);
    CHECK(out.size() == 2 && out[0] == 1 && out[1] == 2);

    /* mixed: packed [1,2] then unpacked 3 (tag 0x15). */
    const uint8_t mixed[] = { 0x12, 0x08, 1, 0, 0, 0, 2, 0, 0, 0, 0x15, 3, 0, 0, 0 };
    std::vector<uint32_t> out2;
    auto h2 = ppb::on_bulk<K::w>(
        [&](std::span<const ppb::le_packed<uint32_t>> view) -> ppb_error
        {
            out2.reserve(out2.size() + view.size());
            for (auto w : view)
            {
                out2.push_back(w.value());
            }

            return PPB_OK;
        },
        [&](uint32_t v) -> ppb_error
        {
            out2.push_back(v);
            return PPB_OK;
        });

    ppb::reader<S> r2(std::as_bytes(std::span(mixed)));
    CHECK(r2.parse(h2) == PPB_OK);
    CHECK((out2 == std::vector<uint32_t> { 1, 2, 3 }));
}

/* check for correct callback order for mixed packed/unpacked encoding. */
static void
test_wire_order_mixed_forms()
{
    enum class K : int
    {
        xs = 1,
        ws = 2
    };
    using S = ppb::auto_schema<ppb::packed_int32<K::xs>, ppb::int32<K::xs, ppb::field_semantics::always_lexn>,
        ppb::packed_sfixed32<K::ws>, ppb::sfixed32<K::ws, ppb::field_semantics::always_lexn>>;

    auto xs = collect_on_each<S, K::xs, int32_t>({ 0x0a, 0x02, 2, 3, 0x08, 1 });
    CHECK((xs == std::vector<int32_t> { 2, 3, 1 })); /* wire order, not [1,2,3] */

    auto only = collect_on_each<S, K::xs, int32_t>({ 0x0a, 0x02, 4, 5 });
    CHECK((only == std::vector<int32_t> { 4, 5 }));

    /* field 2 sfixed32: unpacked 7 (tag 0x15) then packed [8] (tag 0x12 len 4). */
    auto ws = collect_on_each<S, K::ws, int32_t>({ 0x15, 7, 0, 0, 0, 0x12, 0x04, 8, 0, 0, 0 });
    CHECK((ws == std::vector<int32_t> { 7, 8 }));
}

/* more edge cases. */
static void
test_edges_and_equivalence()
{
    enum class K : int
    {
        v = 1
    };
    using S = ppb::auto_schema<ppb::packed_int32<K::v>, ppb::int32<K::v, ppb::field_semantics::always_lexn>>;

    CHECK((collect_on_each<S, K::v, int32_t>({ 0x0a, 0x00 }).empty())); /* empty packed */
    CHECK((collect_on_each<S, K::v, int32_t>({ 0x0a, 0x01, 5 }) ==
        std::vector<int32_t> { 5 })); /* single-element packed */
    CHECK((collect_on_each<S, K::v, int32_t>({ 0x0a, 0x02, 0xac, 0x02 }) /* packed [300] */
        == std::vector<int32_t> { 300 }));

    const uint8_t mixed[] = { 0x0a, 0x01, 5, 0x08, 6 }; /* packed [5], unpacked 6 */

    /* All three readers parse the same `mixed[]` so the differential bites. */
    std::vector<int32_t> via_each;
    ppb::reader<S> re(std::as_bytes(std::span(mixed)));
    (void)re.parse(ppb::on_each<K::v>([&](int32_t v) { via_each.push_back(v); }));

    std::vector<int32_t> via_push;
    ppb::reader<S> rp(std::as_bytes(std::span(mixed)));
    (void)rp.parse(ppb::push_back<K::v>(&via_push));

    std::vector<int32_t> via_manual;
    ppb::reader<S> rm(std::as_bytes(std::span(mixed)));
    (void)rm.parse(ppb::on<K::v>(
        [&](auto v) -> ppb_error
        {
            if constexpr (ppb::detail::is_packed_range_v<std::decay_t<decltype(v)>>)
            {
                for (auto e : v)
                {
                    via_manual.push_back(e);
                }
            }
            else
            {
                via_manual.push_back(v);
            }

            return PPB_OK;
        }));

    CHECK(via_each == via_push);
    CHECK(via_each == via_manual);
}

/* specific wire encoding for on_bulk: ignore vs reject the other encoding. */
static void
test_single_form_policies()
{
    enum class K : int
    {
        v = 1
    };
    using S = ppb::auto_schema<ppb::packed_int32<K::v>, ppb::int32<K::v, ppb::field_semantics::always_lexn>>;

    const uint8_t mixed[] = { 0x0a, 0x01, 5, 0x08, 6 };

    std::vector<int32_t> kept;
    ppb::reader<S> r1(std::as_bytes(std::span(mixed)));
    CHECK(r1.parse(ppb::on_bulk<K::v, ppb::wire_type::len>(
              [&](auto view) -> ppb_error
              {
                  for (auto e : view)
                  {
                      kept.push_back(e);
                  }

                  return PPB_OK;
              },
              [&](int32_t) -> ppb_error { return PPB_OK; })) == PPB_OK);
    CHECK((kept == std::vector<int32_t> { 5 })); /* unpacked 6 was skipped, not collected */

    std::vector<int32_t> got;
    ppb::reader<S> r2(std::as_bytes(std::span(mixed)));
    ppb_error rc = r2.parse(ppb::push_back<K::v, ppb::wire_type::len>(&got),
        ppb::on<K::v, ppb::wire_type::varint>([](auto) -> ppb_error { return PPB_ERROR_CORRUPT_TAG; }));
    CHECK(rc == PPB_ERROR_CORRUPT_TAG); /* unexpected encoding rejected */
}

}  // namespace repeated_tests

int
main()
{
    test_handler_factories();
    test_reader_getters();
    test_reader_default_construct();
    test_reader_span_construct();
    test_reader_ptr_construct();
    test_reader_ptr_construct_parse();
    test_reader_prescan_ok();
    test_reader_prescan_empty();
    test_reader_prescan_with_max_fields();
    test_reader_prescan_with_hard_limit();
    test_reader_prescan_hard_limit_exceeded();
    test_reader_prescan_with_soft_limit();
    test_reader_prescan_twice();
    test_reader_prescan_sticky_error();

    test_reader_prescan_zero_tag();
    test_reader_prescan_corrupt_tag();
    test_reader_prescan_truncated_tag();
    test_reader_prescan_truncated_value();
    test_reader_prescan_valid_then_truncated();
    test_reader_prescan_valid_then_zero_tag();

    test_reader_meta_four_field();
    test_reader_meta_repeated_varint();
    test_reader_meta_repeated_len();
    test_reader_meta_merged();
    test_reader_meta_no_merge();
    test_reader_meta_merged_one_sided();

    test_reader_field_accessor();

    test_reader_prescan_with_handlers();
    test_reader_prescan_handler_error();

    test_reader_lexn_decode_dispatch_consume();
    test_reader_lexn_handler_error_runs_full_batch();
    test_reader_lexn_decode_error_skips_handlers();

    test_lex_all_multi_batch();
    test_lex_all_empty_input();
    test_lex_all_zero_bounds();
    test_lex_all_handler_error();
    test_lex_all_decode_error();
    test_lex_all_sticky_error();
    test_lex_all_per_batch_limit();

    test_reader_parse_happy_path();
    test_reader_parse_init_error_does_not_consume();
    test_reader_parse_prescan_error_skips_init();
    test_reader_parse_handler_error_runs_full_batch();
    test_reader_parse_sticky_error_short_circuits();
    test_parse_bare_handlers();
    test_parse_limit_and_handlers();
    test_parse_init_and_handlers();
    test_parse_limit_init_and_handlers();
    test_reader_parse_per_batch_dispatch();

    test_reader_parse_semantics_error_field_present();
    test_reader_parse_semantics_error_field_absent();
    test_reader_parse_semantics_repeated_single();
    test_reader_parse_semantics_repeated_multi();
    test_reader_parse_semantics_lww_multi();
    test_reader_parse_semantics_proto3_zero_default_absent();
    test_reader_parse_semantics_proto3_zero_default_present();
    test_reader_parse_semantics_proto3_zero_default_multi();
    test_reader_parse_semantics_proto3_zero_default_len_absent();
    test_reader_parse_semantics_proto3_zero_default_in_lexn_path();
    test_proto3_uint32_parse_absent_dispatches_zero();
    test_reader_prescan_semantics_proto3_zero_default_absent();
    test_reader_prescan_semantics_proto3_zero_default_present();
    test_reader_prescan_semantics_lww_absent();
    test_reader_prescan_semantics_lww_and_proto3_absent();
    test_reader_parse_semantics_singular_distinct();
    test_reader_parse_semantics_singular_equal();
    test_reader_parse_semantics_singular_len_multi();
    test_reader_parse_semantics_always_lexn_wire_order();

    test_typed_scalar_varint_fields();
    test_sint32_noncanonical_truncates();
    test_typed_int32_negative_decodes_as_10_byte_varint();
    test_typed_enumerated_field();
    test_typed_scalar_i32_fields();
    test_typed_scalar_i64_fields();
    test_typed_utf8string_field();
    test_typed_bytes_default_element();
    test_typed_bytes_uint32_element();
    test_typed_bytes_misaligned_size_sets_error();
    test_packed_fixed32_misaligned_error_semantics_sticky();
    test_typed_packed_fixed_fields();
    test_typed_packed_sfixed32();
    test_typed_packed_f64();
    test_packed_fixed_alignment_length_sweep();
    test_scalar_value_punning_sweep();
    test_typed_packed_varint_fields();
    test_borrowed_views_alias_input_exact();
    // Empty packed payloads.
    check_empty_payload<ppb::packed_int32<1>>();
    check_empty_payload<ppb::packed_sint32<1>>();
    check_empty_payload<ppb::packed_uint64<1>>();
    check_empty_payload<ppb::packed_boolean<1>>();
    check_empty_payload<ppb::packed_enumerated<1, Color>>();

    // Corrupt packed payload with sticky error already set.
    check_corrupt_after_sticky<ppb::packed_int32<2>>();
    check_corrupt_after_sticky<ppb::packed_sint32<2>>();
    check_corrupt_after_sticky<ppb::packed_uint64<2>>();
    check_corrupt_after_sticky<ppb::packed_boolean<2>>();
    check_corrupt_after_sticky<ppb::packed_enumerated<2, Color>>();

    test_typed_packed_enumerated_field();
    test_typed_packed_varint_truncated_payload();
    test_unpacked_aliases_compile_and_decode();

    test_packed_varint_iter_default_constructed();
    test_merge_meta();
    test_parse_corrupt_underlying_in_init();
    test_dispatch_when_ok_and_when_error();
    test_reuse_reader_without_reset_fields();
    test_parse_empty_prescan();
    test_parse_empty_init_error();
    test_parse_empty_no_init();

    test_dispatch_limit_multiple_of_16();
    test_dispatch_limit_non_multiple_of_16();
    test_dispatch_range_2_40_of_48();
    test_dispatch_early_exit();
    test_dispatch_limit_0();
    test_dispatch_limit_1();
    test_dispatch_high_indices();
    test_dispatch_wide_mixed_wire_boundary();
    test_dispatch_out_of_range_bounds();
    test_wire_type_mismatch_payload_bounded();
    test_lexn_sticky_error();
    test_prescan_sticky_error();
    test_parse_init_error_branch();
    test_reader_too_large_input();
    test_reader_too_large_input_ptr();

    test_unknown_field_baseline_no_catchalls();
    test_unknown_field_detects_via_parse();
    test_unknown_field_detects_via_prescan();
    test_unknown_field_first_write_wins();
    test_unknown_field_sticky_across_reset_fields();
    test_unknown_field_sticky_across_messages();
    test_unknown_field_known_handlers_still_run();
    test_on_unknown_dispatches_per_occurrence();
    test_on_unknown_per_wire_dispatch();
    test_on_unknown_handler_error_propagates();

    submsg_tests::test_on_submessage_two_level();
    submsg_tests::test_on_submessage_three_level_ok_at_depth_3();
    submsg_tests::test_on_submessage_three_level_fails_at_depth_2();
    submsg_tests::test_on_submessage_default_limit_fails_closed();
    submsg_tests::test_on_submessage_inner_handler_error_propagates();
    submsg_tests::test_on_submessage_repeated();
    submsg_tests::test_on_submessage_with_init();
    submsg_tests::test_on_submessage_typed_message_field();
    submsg_tests::test_on_submessage_zero_length_runs_init();
    submsg_tests::test_on_submessage_validates_every_occurrence();

    repeated_tests::test_int32_packed_equals_unpacked();
    repeated_tests::test_sfixed32_packed_equals_unpacked();
    repeated_tests::test_double_packed_equals_unpacked();
    repeated_tests::test_enum_packed_equals_unpacked();
    repeated_tests::test_stateful_handler_across_both_forms();
    repeated_tests::test_stateful_handler_by_value();
    repeated_tests::test_error_short_circuits_packed_loop();
    repeated_tests::test_error_short_circuits_unpacked();
    repeated_tests::test_on_bulk_range_fn_error();
    repeated_tests::test_void_on_each_yields_ok();
    repeated_tests::test_on_bulk_fixed_width();
    repeated_tests::test_wire_order_mixed_forms();
    repeated_tests::test_edges_and_equivalence();
    repeated_tests::test_single_form_policies();

    std::printf("\n%d checks, %d failures\n", g_check_count, g_fail_count);
    return g_fail_count > 0 ? 1 : 0;
}
