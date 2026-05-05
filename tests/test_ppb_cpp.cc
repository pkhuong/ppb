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

    std::printf("\n%d checks, %d failures\n", g_check_count, g_fail_count);
    return g_fail_count > 0 ? 1 : 0;
}
