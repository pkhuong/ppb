#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ppb/ppb.hpp>
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

    ppb::reader<TestSchema> r2;
    CHECK(r2.size() == 0);
    CHECK(r2.empty());

    ppb::limit lim = ppb::limit::hard(100, 10);
    CHECK(lim.bytes() == 100);
    CHECK(lim.fields() == 10);
    CHECK(lim.error_on_bytes() == PPB_ERROR_LIMIT_EXCEEDED);

    // num_fields() at runtime
    CHECK(TestSchema::num_fields() == 4);
}

// parse() with empty input: prescan reads 0 bytes, takes the num_bytes <= 0 path.
static void
test_parse_empty_prescan()
{
    ppb::reader<TestSchema> r(std::span<const std::byte> {});
    CHECK(r.parse([](const ppb::reader<TestSchema> &) -> ppb_error { return PPB_OK; }) == PPB_OK);
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
using KnownPlusUnknownSchema = ppb::auto_schema<ppb::varint<1>, ppb::detect_unknown_fields>;

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

    using S = ppb::auto_schema<ppb::varint<1>, ppb::detect_unknown_fields>;
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

int
main()
{
    test_reader_getters();
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
    test_reader_lexn_decode_error_skips_handlers();

    test_reader_parse_happy_path();
    test_reader_parse_init_error_does_not_consume();
    test_reader_parse_prescan_error_skips_init();
    test_reader_parse_handler_error_runs_full_batch();
    test_reader_parse_sticky_error_short_circuits();
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
    test_reader_prescan_semantics_proto3_zero_default_absent();
    test_reader_prescan_semantics_proto3_zero_default_present();
    test_reader_prescan_semantics_lww_absent();
    test_reader_prescan_semantics_lww_and_proto3_absent();
    test_reader_parse_semantics_singular_distinct();
    test_reader_parse_semantics_singular_equal();
    test_reader_parse_semantics_singular_len_multi();
    test_reader_parse_semantics_always_lexn_wire_order();

    test_typed_scalar_varint_fields();
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
    test_typed_packed_varint_fields();
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
    test_parse_empty_prescan();

    test_dispatch_limit_multiple_of_16();
    test_dispatch_limit_non_multiple_of_16();
    test_dispatch_range_2_40_of_48();
    test_dispatch_early_exit();
    test_dispatch_limit_0();
    test_dispatch_limit_1();
    test_dispatch_high_indices();
    test_lexn_sticky_error();
    test_prescan_sticky_error();
    test_parse_init_error_branch();
    test_reader_too_large_input();

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

    std::printf("\n%d checks, %d failures\n", g_check_count, g_fail_count);
    return g_fail_count > 0 ? 1 : 0;
}
