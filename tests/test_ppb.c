/*
 * Unit tests for ppb internals: ppb_zag, ppb_decode_varint,
 * ppb_prescan, ppb_lexn, and PPB_TAG macros.
 *
 * Build: gcc -std=c2x -O2 -Iinclude/ -Wall -Wextra -Wpedantic -o build/test_ppb tests/test_ppb.c src/ppb.c
 * Run:   build/test_ppb
 */
#include "ppb/ppb.h"
#include "src/varint.h"

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_fail_count;
static int g_check_count;

#define CHECK(cond)                                                           \
    do                                                                        \
    {                                                                         \
        g_check_count++;                                                      \
        if (!(cond))                                                          \
        {                                                                     \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_fail_count++;                                                   \
        }                                                                     \
    } while (0)

static struct ppb_buf
make_buf(const void *data, size_t size)
{
    return (struct ppb_buf) { .buf = data, .size = size };
}

static void
zero_fields(size_t n, struct ppb_field fields[n])
{
    memset(fields, 0, n * sizeof(fields[0]));
}

/* Wire-format buffer shared by prescan/lexn tests that need all 4 wire types. */
static const uint8_t four_field_wire[] = {
    0x08, 0x96, 0x01,                                     /* field 1 varint 150 */
    0x11, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* field 2 i64 1 */
    0x1a, 0x05, 0x68, 0x65, 0x6c, 0x6c, 0x6f,            /* field 3 LEN "hello" */
    0x25, 0x2a, 0x00, 0x00, 0x00,                         /* field 4 i32 42 */
};

static void
init_four_fields(struct ppb_field fields[4])
{
    zero_fields(4, fields);
    fields[0].tag = PPB_TAG(1, PPB_WIRE_VARINT);
    fields[1].tag = PPB_TAG(2, PPB_WIRE_I64);
    fields[2].tag = PPB_TAG(3, PPB_WIRE_LEN);
    fields[3].tag = PPB_TAG(4, PPB_WIRE_I32);
}

static void
test_zag(void)
{
    printf("test_zag\n");
    CHECK(ppb_zag(0) == 0);
    CHECK(ppb_zag(1) == -1);
    CHECK(ppb_zag(2) == 1);
    CHECK(ppb_zag(3) == -2);
    CHECK(ppb_zag(4) == 2);
    CHECK(ppb_zag(UINT64_MAX) == INT64_MIN);
    CHECK(ppb_zag(UINT64_MAX - 1) == INT64_MAX);
}

static void
test_decode_varint(void)
{
    printf("test_decode_varint\n");

    {
        uint8_t data[] = { 0x00 };
        struct ppb_buf buf = make_buf(data, sizeof(data));
        enum ppb_error err = PPB_OK;
        uint64_t val = ppb_decode_varint(&buf, &err);
        CHECK(err == PPB_OK);
        CHECK(val == 0);
        CHECK(buf.size == 0);
    }

    {
        uint8_t data[] = { 0x7f };
        struct ppb_buf buf = make_buf(data, sizeof(data));
        enum ppb_error err = PPB_OK;
        uint64_t val = ppb_decode_varint(&buf, &err);
        CHECK(err == PPB_OK);
        CHECK(val == 127);
        CHECK(buf.size == 0);
    }

    {
        uint8_t data[] = { 0x80, 0x01 };
        struct ppb_buf buf = make_buf(data, sizeof(data));
        enum ppb_error err = PPB_OK;
        uint64_t val = ppb_decode_varint(&buf, &err);
        CHECK(err == PPB_OK);
        CHECK(val == 128);
        CHECK(buf.size == 0);
    }

    {
        uint8_t data[] = { 0x96, 0x01 };
        struct ppb_buf buf = make_buf(data, sizeof(data));
        enum ppb_error err = PPB_OK;
        uint64_t val = ppb_decode_varint(&buf, &err);
        CHECK(err == PPB_OK);
        CHECK(val == 150);
        CHECK(buf.size == 0);
    }

    /* UINT64_MAX */
    {
        uint8_t data[] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01 };
        struct ppb_buf buf = make_buf(data, sizeof(data));
        enum ppb_error err = PPB_OK;
        uint64_t val = ppb_decode_varint(&buf, &err);
        CHECK(err == PPB_OK);
        CHECK(val == UINT64_MAX);
        CHECK(buf.size == 0);
    }

    /* Truncated: continuation byte with no successor. */
    {
        uint8_t data[] = { 0x80 };
        struct ppb_buf buf = make_buf(data, sizeof(data));
        enum ppb_error err = PPB_OK;
        uint64_t val = ppb_decode_varint(&buf, &err);
        CHECK(err == PPB_ERROR_TRUNCATED_DATA);
        CHECK(val == 0);
    }

    /* Corrupt: 10 continuation bytes overflows the 64-bit result. */
    {
        uint8_t data[] = { 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00 };
        struct ppb_buf buf = make_buf(data, sizeof(data));
        enum ppb_error err = PPB_OK;
        uint64_t val = ppb_decode_varint(&buf, &err);
        CHECK(err == PPB_ERROR_CORRUPT_VARINT);
        CHECK(val == 0);
    }

    /* Empty buffer. */
    {
        struct ppb_buf buf = make_buf("", 0);
        enum ppb_error err = PPB_OK;
        uint64_t val = ppb_decode_varint(&buf, &err);
        CHECK(err == PPB_ERROR_TRUNCATED_DATA);
        CHECK(val == 0);
    }

    /* Advances buf past the consumed varint. */
    {
        uint8_t data[] = { 0x96, 0x01, 0x42 };
        struct ppb_buf buf = make_buf(data, sizeof(data));
        enum ppb_error err = PPB_OK;
        uint64_t val = ppb_decode_varint(&buf, &err);
        CHECK(err == PPB_OK);
        CHECK(val == 150);
        CHECK(buf.size == 1);
        CHECK(buf.buf == data + 2);
    }
}

static void
test_prescan_known_fields(void)
{
    printf("test_prescan_known_fields\n");

    struct ppb_field fields[4];
    init_four_fields(fields);

    struct ppb_buf buf = make_buf(four_field_wire, sizeof(four_field_wire));
    ptrdiff_t ret = ppb_prescan(buf, 4, fields, SIZE_MAX);

    CHECK(ret == (ptrdiff_t)sizeof(four_field_wire));

    CHECK(fields[0].m.num_occurrences == 1);
    CHECK(fields[0].m.total_bytes == 2);
    CHECK(fields[0].v.u64 == 150);

    CHECK(fields[1].m.num_occurrences == 1);
    CHECK(fields[1].m.total_bytes == 8);
    CHECK(fields[1].v.u64 == 1);

    CHECK(fields[2].m.num_occurrences == 1);
    CHECK(fields[2].m.total_bytes == 5);
    CHECK(fields[2].v.payload.size == 5);
    CHECK(memcmp(fields[2].v.payload.buf, "hello", 5) == 0);

    CHECK(fields[3].m.num_occurrences == 1);
    CHECK(fields[3].m.total_bytes == 4);
    CHECK(fields[3].v.u64 == 42);
}

static void
test_prescan_repeated_fields(void)
{
    printf("test_prescan_repeated_fields\n");

    static const uint8_t wire[] = {
        0x08, 0x01,        /* field 1 varint 1  (1-byte value) */
        0x08, 0x96, 0x01,  /* field 1 varint 150 (2-byte value) */
        0x08, 0x01,        /* field 1 varint 1  (1-byte value) */
    };

    struct ppb_field fields[1];
    zero_fields(1, fields);
    fields[0].tag = PPB_TAG(1, PPB_WIRE_VARINT);

    struct ppb_buf buf = make_buf(wire, sizeof(wire));
    ptrdiff_t ret = ppb_prescan(buf, 1, fields, SIZE_MAX);

    CHECK(ret == (ptrdiff_t)sizeof(wire));
    CHECK(fields[0].m.num_occurrences == 3);
    CHECK(fields[0].m.total_bytes == 4);
    CHECK(fields[0].m.min_nonzero_bytes == 1);
    CHECK(fields[0].m.max_bytes == 2);
    CHECK(fields[0].v.u64 == 1); /* last value seen */
}

static void
test_prescan_validation(void)
{
    printf("test_prescan_validation\n");

    static const uint8_t wire[] = { 0x08, 0x01 };
    struct ppb_buf buf = make_buf(wire, sizeof(wire));

    /* Unsorted fields. */
    {
        struct ppb_field fields[2];
        zero_fields(2, fields);
        fields[0].tag = PPB_TAG(2, PPB_WIRE_VARINT);
        fields[1].tag = PPB_TAG(1, PPB_WIRE_VARINT);

        ptrdiff_t ret = ppb_prescan(buf, 2, fields, SIZE_MAX);
        CHECK(ret == PPB_ERROR_UNSORTED_FIELD_ARR);
    }

    /* Sentinel (tag.bits == 0). */
    {
        struct ppb_field fields[1];
        zero_fields(1, fields);
        fields[0].tag.bits = 0;

        ptrdiff_t ret = ppb_prescan(buf, 1, fields, SIZE_MAX);
        CHECK(ret == PPB_ERROR_SENTINEL_FIELD_ARR);
    }

    /* Empty fields array succeeds on valid data. */
    {
        ptrdiff_t ret = ppb_prescan(buf, 0, NULL, SIZE_MAX);
        CHECK(ret == (ptrdiff_t)sizeof(wire));
    }
}

/* Sentinel check fires before the sortedness loop. */
static void
test_prescan_sentinel_early_return(void)
{
    printf("test_prescan_sentinel_early_return\n");

    static const uint8_t wire[] = { 0x08, 0x01 };
    struct ppb_buf buf = make_buf(wire, sizeof(wire));

    struct ppb_field fields[2];
    zero_fields(2, fields);
    fields[0].tag.bits = 0;
    fields[1].tag = PPB_TAG(1, PPB_WIRE_VARINT);
    fields[1].m.num_occurrences = 42; /* canary */

    ptrdiff_t ret = ppb_prescan(buf, 2, fields, SIZE_MAX);
    CHECK(ret == PPB_ERROR_SENTINEL_FIELD_ARR);
    CHECK(fields[1].m.num_occurrences == 42);
}

static void
test_prescan_unknown_fields(void)
{
    printf("test_prescan_unknown_fields\n");

    static const uint8_t wire[] = {
        0x08, 0x01,        /* field 1 varint 1 */
        0x98, 0x06, 0x02,  /* field 99 varint 2 */
    };
    struct ppb_buf buf = make_buf(wire, sizeof(wire));

    /* Without catch-all: field 99 silently discarded. */
    {
        struct ppb_field fields[1];
        zero_fields(1, fields);
        fields[0].tag = PPB_TAG(1, PPB_WIRE_VARINT);

        ptrdiff_t ret = ppb_prescan(buf, 1, fields, SIZE_MAX);
        CHECK(ret == (ptrdiff_t)sizeof(wire));
        CHECK(fields[0].m.num_occurrences == 1);
        CHECK(fields[0].v.u64 == 1);
    }

    /* With catch-all varint: field 99 routed there. */
    {
        struct ppb_field fields[2];
        zero_fields(2, fields);
        fields[0].tag = PPB_TAG(1, PPB_WIRE_VARINT);
        fields[1].tag = PPB_TAG(-1, PPB_WIRE_VARINT);

        ptrdiff_t ret = ppb_prescan(buf, 2, fields, SIZE_MAX);
        CHECK(ret == (ptrdiff_t)sizeof(wire));
        CHECK(fields[0].m.num_occurrences == 1);
        CHECK(fields[0].v.u64 == 1);
        CHECK(fields[1].m.num_occurrences == 1);
        CHECK(fields[1].v.u64 == 2);
    }
}

static void
test_prescan_max_fields(void)
{
    printf("test_prescan_max_fields\n");

    static const uint8_t wire[] = {
        0x08, 0x01,  /* field 1 varint 1 */
        0x10, 0x02,  /* field 2 varint 2 */
        0x18, 0x03,  /* field 3 varint 3 */
    };

    struct ppb_field fields[3];
    zero_fields(3, fields);
    fields[0].tag = PPB_TAG(1, PPB_WIRE_VARINT);
    fields[1].tag = PPB_TAG(2, PPB_WIRE_VARINT);
    fields[2].tag = PPB_TAG(3, PPB_WIRE_VARINT);

    struct ppb_buf buf = make_buf(wire, sizeof(wire));
    ptrdiff_t ret = ppb_prescan(buf, 3, fields, 1);

    CHECK(ret == 2);
    CHECK(fields[0].m.num_occurrences == 1);
    CHECK(fields[0].v.u64 == 1);
    CHECK(fields[1].m.num_occurrences == 0);
    CHECK(fields[2].m.num_occurrences == 0);
}

/* 0x00 on the wire → tag 0 → CORRUPT_TAG. */
static void
test_prescan_zero_tag(void)
{
    printf("test_prescan_zero_tag\n");

    static const uint8_t wire[] = { 0x00 };
    struct ppb_buf buf = make_buf(wire, sizeof(wire));

    struct ppb_field fields[1];
    zero_fields(1, fields);
    fields[0].tag = PPB_TAG(1, PPB_WIRE_VARINT);

    ptrdiff_t ret = ppb_prescan(buf, 1, fields, SIZE_MAX);
    CHECK(ret == PPB_ERROR_CORRUPT_TAG);
}

/* 8 continuation bytes → peek_tag fast path, stop_bits==0 → CORRUPT_TAG. */
static void
test_prescan_corrupt_tag(void)
{
    printf("test_prescan_corrupt_tag\n");

    static const uint8_t wire[] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    struct ppb_buf buf = make_buf(wire, sizeof(wire));

    struct ppb_field fields[1];
    zero_fields(1, fields);
    fields[0].tag = PPB_TAG(1, PPB_WIRE_VARINT);

    ptrdiff_t ret = ppb_prescan(buf, 1, fields, SIZE_MAX);
    CHECK(ret == PPB_ERROR_CORRUPT_TAG);
}

/*
 * Single continuation byte, buffer < 8 bytes → slow path → TRUNCATED_DATA.
 * decode_tag returns tag=0 and sets error; prescan's tag==0 check
 * calls error_set again, but it's sticky so the first error wins.
 */
static void
test_prescan_truncated_tag(void)
{
    printf("test_prescan_truncated_tag\n");

    static const uint8_t wire[] = { 0x80 };
    struct ppb_buf buf = make_buf(wire, sizeof(wire));

    struct ppb_field fields[1];
    zero_fields(1, fields);
    fields[0].tag = PPB_TAG(1, PPB_WIRE_VARINT);

    ptrdiff_t ret = ppb_prescan(buf, 1, fields, SIZE_MAX);
    CHECK(ret == PPB_ERROR_TRUNCATED_DATA);
}

/* All 4 wire types, in ascending field order. */
static void
test_lexn_known_fields_in_order(void)
{
    printf("test_lexn_known_fields_in_order\n");

    struct ppb_field fields[4];
    init_four_fields(fields);

    struct ppb_buf buf = make_buf(four_field_wire, sizeof(four_field_wire));
    struct ppb_lexn_ret ret = ppb_lexn(&buf, 4, fields, 4);

    CHECK(ret.status == PPB_OK);
    CHECK(ret.first_field == 0);
    CHECK(ret.field_range == 4);
    CHECK(buf.size == 0);

    CHECK(fields[0].v.u64 == 150);
    CHECK(fields[1].v.u64 == 1);
    CHECK(fields[2].v.payload.size == 5);
    CHECK(memcmp(fields[2].v.payload.buf, "hello", 5) == 0);
    CHECK(fields[3].v.u64 == 42);

    CHECK(fields[0].v.ptr == four_field_wire + 0);
    CHECK(fields[1].v.ptr == four_field_wire + 3);
    CHECK(fields[2].v.ptr == four_field_wire + 12);
    CHECK(fields[3].v.ptr == four_field_wire + 19);
}

/* Same buffer, max_lexed_fields=1 → one field per call. */
static void
test_lexn_one_per_call(void)
{
    printf("test_lexn_one_per_call\n");

    struct ppb_field fields[4];
    init_four_fields(fields);

    struct ppb_buf buf = make_buf(four_field_wire, sizeof(four_field_wire));
    struct ppb_lexn_ret ret;

    ret = ppb_lexn(&buf, 4, fields, 1);
    CHECK(ret.status == PPB_OK);
    CHECK(ret.first_field == 0);
    CHECK(ret.field_range == 1);
    CHECK(fields[0].v.u64 == 150);

    ret = ppb_lexn(&buf, 4, fields, 1);
    CHECK(ret.status == PPB_OK);
    CHECK(ret.first_field == 1);
    CHECK(ret.field_range == 1);
    CHECK(fields[1].v.u64 == 1);

    ret = ppb_lexn(&buf, 4, fields, 1);
    CHECK(ret.status == PPB_OK);
    CHECK(ret.first_field == 2);
    CHECK(ret.field_range == 1);
    CHECK(fields[2].v.payload.size == 5);

    ret = ppb_lexn(&buf, 4, fields, 1);
    CHECK(ret.status == PPB_OK);
    CHECK(ret.first_field == 3);
    CHECK(ret.field_range == 1);
    CHECK(fields[3].v.u64 == 42);

    /* EOF */
    ret = ppb_lexn(&buf, 4, fields, 1);
    CHECK(ret.status == PPB_OK);
    CHECK(ret.field_range == 0);
}

/* Make sure we correctly no-op when we want 0 field/ */
static void
test_lexn_no_field(void)
{
    printf("test_lexn_no_field\n");

    struct ppb_buf buf = make_buf(four_field_wire, sizeof(four_field_wire));
    struct ppb_lexn_ret ret = ppb_lexn(&buf, 0, NULL, 4);

    CHECK(ret.status == PPB_OK);
    CHECK(ret.first_field == 0);
    CHECK(ret.field_range == 0);
    CHECK(buf.size == 0);
}

/* catch-all fields are always one at a time. */
static void
test_lexn_catchall_fields_in_order(void)
{
    printf("test_lexn_catchall_fields_in_order\n");
    enum
    {
        NUM_FIELDS = 4
    };
    struct ppb_field fields[NUM_FIELDS] = {
        { .tag = PPB_TAG(-1, PPB_WIRE_VARINT) },
        { .tag = PPB_TAG(-1, PPB_WIRE_I64) },
        { .tag = PPB_TAG(-1, PPB_WIRE_LEN) },
        { .tag = PPB_TAG(-1, PPB_WIRE_I32) },
    };

    struct ppb_buf buf = make_buf(four_field_wire, sizeof(four_field_wire));

    {
        struct ppb_lexn_ret ret = ppb_lexn(&buf, NUM_FIELDS, fields, SIZE_MAX);

        CHECK(ret.status == PPB_OK);
        // varint
        CHECK(ret.first_field == 0);
        CHECK(ret.field_range == 1);
        CHECK(fields[0].v.u64 == 150);
    }

    {
        struct ppb_lexn_ret ret = ppb_lexn(&buf, NUM_FIELDS, fields, SIZE_MAX);

        CHECK(ret.status == PPB_OK);
        // i64
        CHECK(ret.first_field == 1);
        CHECK(ret.field_range == 1);
        CHECK(fields[1].v.u64 == 1);
    }

    {
        struct ppb_lexn_ret ret = ppb_lexn(&buf, NUM_FIELDS, fields, SIZE_MAX);

        CHECK(ret.status == PPB_OK);
        // len
        CHECK(ret.first_field == 2);
        CHECK(ret.field_range == 1);
        CHECK(fields[2].v.payload.size == 5);
        CHECK(memcmp(fields[2].v.payload.buf, "hello", 5) == 0);
    }

    {
        struct ppb_lexn_ret ret = ppb_lexn(&buf, NUM_FIELDS, fields, SIZE_MAX);

        CHECK(ret.status == PPB_OK);
        // i32
        CHECK(ret.first_field == 3);
        CHECK(ret.field_range == 1);
        CHECK(fields[3].v.u64 == 42);
    }

    /* EOF */
    {
        struct ppb_lexn_ret ret = ppb_lexn(&buf, NUM_FIELDS, fields, SIZE_MAX);

        CHECK(ret.status == PPB_OK);
        CHECK(ret.first_field == 0);
        CHECK(ret.field_range == 0);
    }

    {
        struct ppb_lexn_ret ret = ppb_lexn(&buf, NUM_FIELDS, fields, SIZE_MAX);

        CHECK(ret.status == PPB_OK);
        CHECK(ret.first_field == 0);
        CHECK(ret.field_range == 0);
    }
}

/* Field 2 then field 1 → nonmonotonic, stops after each. */
static void
test_lexn_out_of_order(void)
{
    printf("test_lexn_out_of_order\n");

    static const uint8_t wire[] = {
        0x10, 0x02,  /* field 2 varint 2 */
        0x08, 0x01,  /* field 1 varint 1 */
    };

    struct ppb_field fields[2];
    zero_fields(2, fields);
    fields[0].tag = PPB_TAG(1, PPB_WIRE_VARINT);
    fields[1].tag = PPB_TAG(2, PPB_WIRE_VARINT);

    struct ppb_buf buf = make_buf(wire, sizeof(wire));
    struct ppb_lexn_ret ret;

    ret = ppb_lexn(&buf, 2, fields, 4);
    CHECK(ret.status == PPB_OK);
    CHECK(ret.first_field == 1);
    CHECK(ret.field_range == 1);
    CHECK(fields[1].v.u64 == 2);

    ret = ppb_lexn(&buf, 2, fields, 4);
    CHECK(ret.status == PPB_OK);
    CHECK(ret.first_field == 0);
    CHECK(ret.field_range == 1);
    CHECK(fields[0].v.u64 == 1);

    ret = ppb_lexn(&buf, 2, fields, 4);
    CHECK(ret.field_range == 0);
}

/*
 * Unknown field 99 (no catch-all) is silently consumed into dummy;
 * prev_tag_id is NOT updated, so field 3 is still monotonic.
 * One call returns both field 1 and field 3.
 */
static void
test_lexn_unknown_field_skipped(void)
{
    printf("test_lexn_unknown_field_skipped\n");

    static const uint8_t wire[] = {
        0x08, 0x01,        /* field 1 varint 1 */
        0x98, 0x06, 0x02,  /* field 99 varint 2 (unknown) */
        0x18, 0x03,        /* field 3 varint 3 */
    };

    struct ppb_field fields[2];
    zero_fields(2, fields);
    fields[0].tag = PPB_TAG(1, PPB_WIRE_VARINT);
    fields[1].tag = PPB_TAG(3, PPB_WIRE_VARINT);

    struct ppb_buf buf = make_buf(wire, sizeof(wire));
    struct ppb_lexn_ret ret = ppb_lexn(&buf, 2, fields, 4);

    CHECK(ret.status == PPB_OK);
    CHECK(ret.first_field == 0);
    CHECK(ret.field_range == 2);
    CHECK(fields[0].v.u64 == 1);
    CHECK(fields[1].v.u64 == 3);
    CHECK(buf.size == 0);
}

static void
test_lexn_unknown_field_in_order_skipped(void)
{
    printf("test_lexn_unknown_field_in_order_skipped\n");

    static const uint8_t wire[] = {
        0x08, 0x01,        /* field 1 varint 1 */
        0x10, 0x02,        /* field 2 varint 2 (unknown) */
        0x18, 0x03,        /* field 3 varint 3 */
    };

    struct ppb_field fields[2];
    zero_fields(2, fields);
    fields[0].tag = PPB_TAG(1, PPB_WIRE_VARINT);
    fields[1].tag = PPB_TAG(3, PPB_WIRE_VARINT);

    struct ppb_buf buf = make_buf(wire, sizeof(wire));
    struct ppb_lexn_ret ret = ppb_lexn(&buf, 2, fields, 4);

    CHECK(ret.status == PPB_OK);
    CHECK(ret.first_field == 0);
    CHECK(ret.field_range == 2);
    CHECK(fields[0].v.u64 == 1);
    CHECK(fields[1].v.u64 == 3);
    CHECK(buf.size == 0);
}

static void
test_lexn_unknown_field_in_order_skipped_at_end(void)
{
    printf("test_lexn_unknown_field_in_order_skipped_at_end\n");

    static const uint8_t wire[] = {
        0x08, 0x01,        /* field 1 varint 1 */
        0x18, 0x03,        /* field 3 varint 3 */
        0x98, 0x06, 0x02,  /* field 99 varint 2 (unknown) */
    };

    struct ppb_field fields[2];
    zero_fields(2, fields);
    fields[0].tag = PPB_TAG(1, PPB_WIRE_VARINT);
    fields[1].tag = PPB_TAG(3, PPB_WIRE_VARINT);

    struct ppb_buf buf = make_buf(wire, sizeof(wire));
    struct ppb_lexn_ret ret = ppb_lexn(&buf, 2, fields, 4);

    CHECK(ret.status == PPB_OK);
    CHECK(ret.first_field == 0);
    CHECK(ret.field_range == 2);
    CHECK(fields[0].v.u64 == 1);
    CHECK(fields[1].v.u64 == 3);
    CHECK(buf.size == 0);
}

/*
 * Two ascending runs: [field 1, field 3] then [field 2, field 4].
 * Field 2 is nonmonotonic after field 3 → stops between the runs.
 */
static void
test_lexn_two_sorted_runs(void)
{
    printf("test_lexn_two_sorted_runs\n");

    static const uint8_t wire[] = {
        0x08, 0x0a,  /* field 1 varint 10 */
        0x18, 0x1e,  /* field 3 varint 30 */
        0x10, 0x14,  /* field 2 varint 20 — nonmonotonic */
        0x20, 0x28,  /* field 4 varint 40 */
    };

    struct ppb_field fields[4];
    zero_fields(4, fields);
    fields[0].tag = PPB_TAG(1, PPB_WIRE_VARINT);
    fields[1].tag = PPB_TAG(2, PPB_WIRE_VARINT);
    fields[2].tag = PPB_TAG(3, PPB_WIRE_VARINT);
    fields[3].tag = PPB_TAG(4, PPB_WIRE_VARINT);

    struct ppb_buf buf = make_buf(wire, sizeof(wire));
    struct ppb_lexn_ret ret;

    ret = ppb_lexn(&buf, 4, fields, 4);
    CHECK(ret.status == PPB_OK);
    CHECK(ret.first_field == 0);
    CHECK(ret.field_range == 3); /* idx 0..2 */
    CHECK(fields[0].v.u64 == 10);
    CHECK(fields[2].v.u64 == 30);

    ret = ppb_lexn(&buf, 4, fields, 4);
    CHECK(ret.status == PPB_OK);
    CHECK(ret.first_field == 1);
    CHECK(ret.field_range == 3); /* idx 1..3 */
    CHECK(fields[1].v.u64 == 20);
    CHECK(fields[3].v.u64 == 40);
    CHECK(buf.size == 0);
}

/*
 * After a catch-all match, prev_tag_id becomes huge (~UINT64_MAX/8),
 * so the next field is always nonmonotonic.  This holds even when
 * the wire types would otherwise be ascending (varint=0 < i32=5).
 */
static void
test_lexn_catchall_always_stops(void)
{
    printf("test_lexn_catchall_always_stops\n");

    static const uint8_t wire[] = {
        0x08, 0x2a,                          /* field 1 varint 42 */
        0x98, 0x06, 0x07,                    /* field 99 varint 7 → catch-all varint */
        0xa5, 0x06, 0x01, 0x00, 0x00, 0x00,  /* field 100 i32 1 → catch-all i32 */
    };

    struct ppb_field fields[3];
    zero_fields(3, fields);
    fields[0].tag = PPB_TAG(1, PPB_WIRE_VARINT);
    fields[1].tag = PPB_TAG(-1, PPB_WIRE_VARINT);
    fields[2].tag = PPB_TAG(-1, PPB_WIRE_I32);

    struct ppb_buf buf = make_buf(wire, sizeof(wire));
    struct ppb_lexn_ret ret;

    ret = ppb_lexn(&buf, 3, fields, 4);
    CHECK(ret.status == PPB_OK);
    CHECK(ret.first_field == 0);
    CHECK(ret.field_range == 2);
    CHECK(fields[0].v.u64 == 42);
    CHECK(fields[1].v.u64 == 7);

    ret = ppb_lexn(&buf, 3, fields, 4);
    CHECK(ret.status == PPB_OK);
    CHECK(ret.first_field == 2);
    CHECK(ret.field_range == 1);
    CHECK(fields[2].v.u64 == 1);

    ret = ppb_lexn(&buf, 3, fields, 4);
    CHECK(ret.field_range == 0);
}

static void
test_lexn_empty(void)
{
    printf("test_lexn_empty\n");

    struct ppb_field fields[1];
    zero_fields(1, fields);
    fields[0].tag = PPB_TAG(1, PPB_WIRE_VARINT);

    struct ppb_buf buf = make_buf("", 0);
    struct ppb_lexn_ret ret = ppb_lexn(&buf, 1, fields, 4);

    CHECK(ret.status == PPB_OK);
    CHECK(ret.first_field == 0);
    CHECK(ret.field_range == 0);
}

/* Tag decoded successfully, but no varint value follows. */
static void
test_lexn_error(void)
{
    printf("test_lexn_error\n");

    static const uint8_t wire[] = { 0x08 };

    struct ppb_field fields[1];
    zero_fields(1, fields);
    fields[0].tag = PPB_TAG(1, PPB_WIRE_VARINT);

    struct ppb_buf buf = make_buf(wire, sizeof(wire));
    struct ppb_lexn_ret ret = ppb_lexn(&buf, 1, fields, 4);

    CHECK(ret.status == PPB_ERROR_TRUNCATED_DATA);
}

/*
 * tag == 0 sets CORRUPT_TAG, regardless of the monotonicity check.
 */
static void
test_lexn_zero_tag(void)
{
    printf("test_lexn_zero_tag\n");

    static const uint8_t wire[] = { 0x00 };
    struct ppb_buf buf = make_buf(wire, sizeof(wire));

    struct ppb_field fields[1];
    zero_fields(1, fields);
    fields[0].tag = PPB_TAG(1, PPB_WIRE_VARINT);

    struct ppb_lexn_ret ret = ppb_lexn(&buf, 1, fields, 4);
    CHECK(ret.status == PPB_ERROR_CORRUPT_TAG);
    CHECK(ret.field_range == 0);
}

/* 8 continuation bytes is corrupt, via the peek_tag fast path. */
static void
test_lexn_corrupt_tag(void)
{
    printf("test_lexn_corrupt_tag\n");

    static const uint8_t wire[] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    struct ppb_buf buf = make_buf(wire, sizeof(wire));

    struct ppb_field fields[1];
    zero_fields(1, fields);
    fields[0].tag = PPB_TAG(1, PPB_WIRE_VARINT);

    struct ppb_lexn_ret ret = ppb_lexn(&buf, 1, fields, 4);
    CHECK(ret.status == PPB_ERROR_CORRUPT_TAG);
    CHECK(ret.field_range == 0);
}

/* Single continuation byte, TRUNCATED_DATA via the slow path. */
static void
test_lexn_truncated_tag(void)
{
    printf("test_lexn_truncated_tag\n");

    static const uint8_t wire[] = { 0x80 };
    struct ppb_buf buf = make_buf(wire, sizeof(wire));

    struct ppb_field fields[1];
    zero_fields(1, fields);
    fields[0].tag = PPB_TAG(1, PPB_WIRE_VARINT);

    struct ppb_lexn_ret ret = ppb_lexn(&buf, 1, fields, 4);
    CHECK(ret.status == PPB_ERROR_TRUNCATED_DATA);
    CHECK(ret.field_range == 0);
}

/*
 * Field 1 decoded, then zero tag on the same call.  lexn reports
 * CORRUPT_TAG alongside the successfully-decoded field (field_range=1)
 * and does NOT advance buf past the zero byte.
 */
static void
test_lexn_zero_tag_after_valid(void)
{
    printf("test_lexn_zero_tag_after_valid\n");

    static const uint8_t wire[] = {
        0x08, 0x01,  /* field 1 varint 1 */
        0x00,        /* zero tag */
    };

    struct ppb_field fields[1];
    zero_fields(1, fields);
    fields[0].tag = PPB_TAG(1, PPB_WIRE_VARINT);

    struct ppb_buf buf = make_buf(wire, sizeof(wire));
    struct ppb_lexn_ret ret;

    ret = ppb_lexn(&buf, 1, fields, 4);
    CHECK(ret.status == PPB_ERROR_CORRUPT_TAG);
    CHECK(ret.first_field == 0);
    CHECK(ret.field_range == 1);
    CHECK(fields[0].v.u64 == 1);
    CHECK(buf.size == 1);

    ret = ppb_lexn(&buf, 1, fields, 4);
    CHECK(ret.status == PPB_ERROR_CORRUPT_TAG);
    CHECK(ret.field_range == 0);
}

static void
test_tag_macros(void)
{
    printf("test_tag_macros\n");

    CHECK(PPB_TAG(1, PPB_WIRE_VARINT).bits == 0x08);
    CHECK(PPB_TAG(1, PPB_WIRE_I64).bits == 0x09);
    CHECK(PPB_TAG(1, PPB_WIRE_LEN).bits == 0x0A);
    CHECK(PPB_TAG(2, PPB_WIRE_VARINT).bits == 0x10);
    CHECK(PPB_TAG(15, PPB_WIRE_VARINT).bits == 0x78);
    CHECK(PPB_TAG(16, PPB_WIRE_VARINT).bits == 0x0180);
    CHECK(PPB_TAG(-1, PPB_WIRE_VARINT).bits == ((UINT64_MAX << 3) | 0));
    CHECK(PPB_TAG(-1, PPB_WIRE_I32).bits == ((UINT64_MAX << 3) | 5));
}

/*
 * PPB_TAG_BITS encodes (field<<3|wire) as a varint in a uint64_t.
 * Decoding those bytes with ppb_decode_varint must recover the
 * original (field<<3|wire) value.
 */
static void
test_tag_varint_roundtrip(void)
{
    printf("test_tag_varint_roundtrip\n");

    static const uint64_t field_numbers[] = {
        1, 2, 15,       /* single-byte tags */
        16, 100,        /* two-byte tags */
        2048,           /* three-byte tag */
        262144,         /* four-byte tag */
        33554432UL,     /* five-byte tag */
    };
    static const enum ppb_wire_type wire_types[] = {
        PPB_WIRE_VARINT,
        PPB_WIRE_I64,
        PPB_WIRE_LEN,
        PPB_WIRE_I32,
    };

    for (size_t fi = 0; fi < sizeof(field_numbers) / sizeof(field_numbers[0]); fi++)
    {
        for (size_t wi = 0; wi < sizeof(wire_types) / sizeof(wire_types[0]); wi++)
        {
            uint64_t fn = field_numbers[fi];
            enum ppb_wire_type wt = wire_types[wi];
            uint64_t expected = (fn << 3) | (uint64_t)wt;

            uint64_t encoded = PPB_TAG_BITS(fn, wt);

            uint8_t tag_bytes[8];
            memcpy(tag_bytes, &encoded, sizeof(tag_bytes));

            struct ppb_buf buf = make_buf(tag_bytes, sizeof(tag_bytes));

            {
                uint64_t decoded_tag;
                int rc = peek_tag(buf, &decoded_tag);
                CHECK(rc > 0);
                CHECK(decoded_tag == encoded);
            }

            {
                uint64_t decoded_tag;
                int rc = peek_varint_slow(buf, &decoded_tag, /*limb_width=*/8, PPB_ERROR_CORRUPT_TAG);
                CHECK(rc > 0);
                CHECK(decoded_tag == encoded);
            }

            {
                uint64_t varint;
                int rc = peek_varint(buf, &varint);
                CHECK(rc > 0);
                CHECK(varint == expected);
            }

            {
                uint64_t varint;
                int rc = peek_varint_slow(buf, &varint, /*limb_width=*/7, PPB_ERROR_CORRUPT_VARINT);
                CHECK(rc > 0);
                CHECK(varint == expected);
            }

            enum ppb_error err = PPB_OK;
            uint64_t decoded = ppb_decode_varint(&buf, &err);

            CHECK(err == PPB_OK);
            CHECK(decoded == expected);
        }
    }
}

int
main(void)
{
    test_zag();
    test_decode_varint();

    test_prescan_known_fields();
    test_prescan_repeated_fields();
    test_prescan_validation();
    test_prescan_sentinel_early_return();
    test_prescan_unknown_fields();
    test_prescan_max_fields();
    test_prescan_zero_tag();
    test_prescan_corrupt_tag();
    test_prescan_truncated_tag();

    test_lexn_known_fields_in_order();
    test_lexn_one_per_call();
    test_lexn_no_field();
    test_lexn_catchall_fields_in_order();
    test_lexn_out_of_order();
    test_lexn_unknown_field_skipped();
    test_lexn_unknown_field_in_order_skipped();
    test_lexn_unknown_field_in_order_skipped_at_end();
    test_lexn_two_sorted_runs();
    test_lexn_catchall_always_stops();
    test_lexn_empty();
    test_lexn_error();
    test_lexn_zero_tag();
    test_lexn_corrupt_tag();
    test_lexn_truncated_tag();
    test_lexn_zero_tag_after_valid();

    test_tag_macros();
    test_tag_varint_roundtrip();

    printf("\n%d checks, %d failures\n", g_check_count, g_fail_count);
    return g_fail_count > 0 ? 1 : 0;
}
