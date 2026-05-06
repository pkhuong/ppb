#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ppb/ppb.hpp>

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

// Prescan handler dispatch tests

enum class HandlerKey : int
{
    x = 1,
    absent = 42
};

using HandlerSchema = ppb::schema<ppb::varint<HandlerKey::x>, ppb::len<HandlerKey::x>>;

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

int
main()
{
    test_reader_default_construct();
    test_reader_span_construct();
    test_reader_ptr_construct();
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

    test_reader_prescan_with_handlers();
    test_reader_prescan_handler_error();

    test_reader_lexn_decode_dispatch_consume();
    test_reader_lexn_handler_error_runs_full_batch();

    std::printf("\n%d checks, %d failures\n", g_check_count, g_fail_count);
    return g_fail_count > 0 ? 1 : 0;
}
