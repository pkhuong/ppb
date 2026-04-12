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

static int g_fail_count = 0;
static int g_check_count = 0;

static inline void
check(bool passes, const char *file, int line, const char *cond)
{
    g_check_count++;
    if (passes)
        return;

    fprintf(stderr, "  FAIL %s:%d: %s\n", file, line, cond);
    g_fail_count++;
    return;
}

#define CHECK(cond) check(!!(cond), __FILE__, __LINE__, #cond)

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
init_four_tags(struct ppb_encoded_tag tags[static 4])
{
    struct ppb_encoded_tag init[4] = {
        PPB_TAG(1, PPB_WIRE_VARINT),
        PPB_TAG(2, PPB_WIRE_I64),
        PPB_TAG(3, PPB_WIRE_LEN),
        PPB_TAG(4, PPB_WIRE_I32),
    };
    memcpy(tags, init, sizeof(init));
}

static void
init_four_fields(struct ppb_field fields[static 4])
{
    zero_fields(4, fields);
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
    /*
     * Admitted postcondition (ppb.c ppb_zag sint32 behavior): for x < 2^32
     * the result fits in int32.  Check boundary values:
     *   UINT32_MAX-1 encodes INT32_MAX (most positive sint32)
     *   UINT32_MAX   encodes INT32_MIN (most negative sint32)
     */
    CHECK(ppb_zag(UINT32_MAX - 1) == INT32_MAX);
    CHECK(ppb_zag(UINT32_MAX) == (int64_t)INT32_MIN);
}

static void
test_peekn(void)
{
    printf("test_peekn\n");

    {
        uint8_t data[] = { 0x13 };
        struct ppb_buf buf = make_buf(data, sizeof(data));
        CHECK(buf_peek8(buf) == 0x13U);
    }

    {
        uint8_t data[] = { 0x80 };
        struct ppb_buf buf = make_buf(data, sizeof(data));
        /* shouldn't sign extend */
        CHECK(buf_peek8(buf) == 0x80U);
    }

    /* should reassemble in little-endian */
    {
        uint8_t data[] = { 0x01, 0x02, 0x03, 0x04 };
        struct ppb_buf buf = make_buf(data, sizeof(data));
        CHECK(buf_peek32(buf) == 0x04030201UL);
    }

    {
        uint8_t data[] = { 0xf1, 0x02, 0x03, 0xf4 };
        struct ppb_buf buf = make_buf(data, sizeof(data));
        /* don't sign-extend */
        CHECK(buf_peek32(buf) == 0xf40302f1UL);
    }

    {
        uint8_t data[] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 };
        struct ppb_buf buf = make_buf(data, sizeof(data));
        CHECK(buf_peek64(buf) == 0x0807060504030201UL);
    }

    {
        uint8_t data[] = { 0x01, 0x02, 0xf3, 0x04, 0x05, 0x06, 0xf7, 0x88 };
        struct ppb_buf buf = make_buf(data, sizeof(data));
        /* don't sign-extend */
        CHECK(buf_peek64(buf) == 0x88f7060504f30201UL);
    }
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
test_peek_varint_error(void)
{
    printf("test_peek_varint_error\n");

    /* Should zero out `val` on error */
    {
        uint8_t data[] = { 0x80 };
        struct ppb_buf buf = make_buf(data, sizeof(data));
        uint64_t val = 42;
        int rc = peek_varint(buf, &val);
        CHECK(rc == PPB_ERROR_TRUNCATED_DATA);
        CHECK(val == 0);
    }

    {
        uint8_t data[] = { 0x80 };
        struct ppb_buf buf = make_buf(data, sizeof(data));
        uint64_t val = 42;
        int rc = peek_varint_slow(buf, &val, 7, PPB_ERROR_CORRUPT_VARINT);
        CHECK(rc == PPB_ERROR_TRUNCATED_DATA);
        CHECK(val == 0);
    }

    {
        uint8_t data[] = { 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80 };
        struct ppb_buf buf = make_buf(data, sizeof(data));
        uint64_t val = 42;
        int rc = peek_varint(buf, &val);
        CHECK(rc == PPB_ERROR_TRUNCATED_DATA);
        CHECK(val == 0);
    }

    {
        uint8_t data[] = { 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80 };
        struct ppb_buf buf = make_buf(data, sizeof(data));
        uint64_t val = 42;
        int rc = peek_varint_slow(buf, &val, 7, PPB_ERROR_CORRUPT_VARINT);
        CHECK(rc == PPB_ERROR_TRUNCATED_DATA);
        CHECK(val == 0);
    }
}

static void
test_peek_tag_error(void)
{
    printf("test_peek_tag_error\n");

    /* Should zero out `val` on error */
    {
        uint8_t data[] = { 0x80 };
        struct ppb_buf buf = make_buf(data, sizeof(data));
        uint64_t val = 123;
        int rc = peek_tag(buf, &val);
        CHECK(rc == PPB_ERROR_TRUNCATED_DATA);
        CHECK(val == 0);
    }

    {
        uint8_t data[] = { 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80 };
        struct ppb_buf buf = make_buf(data, sizeof(data));
        uint64_t val = 123;
        int rc = peek_tag(buf, &val);
        CHECK(rc == PPB_ERROR_CORRUPT_TAG);
        CHECK(val == 0);
    }
}

/*
 * Admitted postcondition (varint.h peek_tag): a successfully decoded tag is
 * always < 2^63.  Exercise the maximum value reachable on each code path,
 * and confirm that the two inputs that would violate the bound — all 0xFF
 * (no stop bit at all) and 0x80 in byte 7 (continuation bit in the last
 * fast-path byte) — are correctly rejected as CORRUPT_TAG.
 *
 * Fast path (>=8 bytes): 7 bytes with continuation bit set, 8th byte with
 * high bit clear — all 63 non-high-bit positions set → 0x7FFFFFFFFFFFFFFF.
 *
 * Slow path (<8 bytes): reads one byte at a time until a byte with high bit
 * clear; a single 0x7F byte gives the max single-byte value 127, also < 2^63.
 *
 * Error cases use 8 bytes of tail padding so the fast path is unambiguously
 * exercised (buf.size > 8), and then call peek_varint_slow explicitly to
 * cover the slow path too.
 */
static void
test_peek_tag_range(void)
{
    printf("test_peek_tag_range\n");

    /* Fast path success: stop bit in byte 7 → result = 0x7FFFFFFFFFFFFFFF. */
    {
        uint8_t data[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F };
        struct ppb_buf buf = make_buf(data, sizeof(data));
        uint64_t val = 0;
        int rc = peek_tag(buf, &val);
        CHECK(rc == 8);
        CHECK(val == (UINT64_MAX >> 1));  /* 2^63 - 1 */
        CHECK(val < ((uint64_t)1 << 63));
    }

    /* Slow path success: single byte 0x7F → result = 127. */
    {
        uint8_t data[] = { 0x7F };
        struct ppb_buf buf = make_buf(data, sizeof(data));
        uint64_t val = 0;
        int rc = peek_tag(buf, &val);
        CHECK(rc == 1);
        CHECK(val == 0x7F);
        CHECK(val < ((uint64_t)1 << 63));
    }

    /*
     * All 8 bytes are 0xFF: no stop bit anywhere in the fast-path window.
     * 8 bytes of 0x00 tail padding ensure buf.size > 8 so the fast path
     * is taken; the error is detected purely from the first 8 bytes.
     */
    {
        uint8_t data[16] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00 };

        uint64_t val = 123;
        int rc = peek_tag(make_buf(data, sizeof(data)), &val);
        CHECK(rc == PPB_ERROR_CORRUPT_TAG);
        CHECK(val == 0);

        /* Slow path: 8 bytes with continuation bits, loop exhausted. */
        val = 123;
        rc = peek_varint_slow(make_buf(data, 8), &val, /*limb_width=*/8, PPB_ERROR_CORRUPT_TAG);
        CHECK(rc == PPB_ERROR_CORRUPT_TAG);
        CHECK(val == 0);
    }

    /*
     * 0x80 in byte 7 (the most-significant fast-path byte): 0x80 = 10000000,
     * the high (continuation) bit is set, so there is still no stop bit in
     * the 8-byte window.  If accepted, the decoded value would be ≥ 2^63.
     */
    {
        uint8_t data[16] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00 };

        uint64_t val = 123;
        int rc = peek_tag(make_buf(data, sizeof(data)), &val);
        CHECK(rc == PPB_ERROR_CORRUPT_TAG);
        CHECK(val == 0);

        /* Slow path: 0x80 has the continuation bit set, same outcome. */
        val = 123;
        rc = peek_varint_slow(make_buf(data, 8), &val, /*limb_width=*/8, PPB_ERROR_CORRUPT_TAG);
        CHECK(rc == PPB_ERROR_CORRUPT_TAG);
        CHECK(val == 0);
    }
}

static void
test_prescan_known_fields(void)
{
    printf("test_prescan_known_fields\n");

    struct ppb_encoded_tag tags[4];
    init_four_tags(tags);
    struct ppb_field fields[4];
    init_four_fields(fields);

    struct ppb_buf buf = make_buf(four_field_wire, sizeof(four_field_wire));
    ptrdiff_t ret = ppb_prescan(buf, 4, tags, fields, SIZE_MAX);

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

    struct ppb_encoded_tag tags[1] = { PPB_TAG(1, PPB_WIRE_VARINT) };
    struct ppb_field fields[1];
    zero_fields(1, fields);

    struct ppb_buf buf = make_buf(wire, sizeof(wire));
    ptrdiff_t ret = ppb_prescan(buf, 1, tags, fields, SIZE_MAX);

    CHECK(ret == (ptrdiff_t)sizeof(wire));
    CHECK(fields[0].m.num_occurrences == 3);
    CHECK(fields[0].m.total_bytes == 4);
    CHECK(fields[0].m.min_nonzero_bytes == 1);
    CHECK(fields[0].m.max_bytes == 2);
    CHECK(fields[0].v.u64 == 1); /* last value seen */
}

static void
test_prescan_repeated_len(void)
{
    printf("test_prescan_repeated_len\n");

    static const uint8_t wire[] = {
        0x0a, 0x00,        /* field 1 len 0 */
        0x0a, 0x03, 0x00, 0x01, 0x02,  /* field 1 len 3 */
        0x0a, 0x02, 0x00, 0x01,  /* field 1 len 2 */
        0x0a, 0x00,        /* field 1 len 0 */
    };

    struct ppb_encoded_tag tags[1] = { PPB_TAG(1, PPB_WIRE_LEN) };
    struct ppb_field fields[1];
    zero_fields(1, fields);

    struct ppb_buf buf = make_buf(wire, sizeof(wire));
    ptrdiff_t ret = ppb_prescan(buf, 1, tags, fields, SIZE_MAX);

    CHECK(ret == (ptrdiff_t)sizeof(wire));
    CHECK(fields[0].m.num_occurrences == 4);
    CHECK(fields[0].m.total_bytes == 5);
    CHECK(fields[0].m.min_nonzero_bytes == 2);
    CHECK(fields[0].m.max_bytes == 3);
}

static void
test_prescan_validation(void)
{
    printf("test_prescan_validation\n");

    static const uint8_t wire[] = { 0x08, 0x01 };
    struct ppb_buf buf = make_buf(wire, sizeof(wire));

    /* Unsorted fields. */
    {
        struct ppb_encoded_tag tags[2] = { PPB_TAG(2, PPB_WIRE_VARINT), PPB_TAG(1, PPB_WIRE_VARINT) };
        struct ppb_field fields[2];
        zero_fields(2, fields);

        ptrdiff_t ret = ppb_prescan(buf, 2, tags, fields, SIZE_MAX);
        CHECK(ret == PPB_ERROR_UNSORTED_FIELD_ARR);
    }

    /* Unsorted field after sorted run. */
    {
        struct ppb_encoded_tag tags[4] = {
            PPB_TAG(1, PPB_WIRE_VARINT),
            PPB_TAG(2, PPB_WIRE_VARINT),
            PPB_TAG(4, PPB_WIRE_VARINT),
            PPB_TAG(3, PPB_WIRE_VARINT),
        };
        struct ppb_field fields[4];
        zero_fields(4, fields);

        ptrdiff_t ret = ppb_prescan(buf, 4, tags, fields, SIZE_MAX);
        CHECK(ret == PPB_ERROR_UNSORTED_FIELD_ARR);
    }

    /* Repeated field. */
    {
        struct ppb_encoded_tag tags[2] = { PPB_TAG(1, PPB_WIRE_VARINT), PPB_TAG(1, PPB_WIRE_VARINT) };
        struct ppb_field fields[2];
        zero_fields(2, fields);

        ptrdiff_t ret = ppb_prescan(buf, 2, tags, fields, SIZE_MAX);
        CHECK(ret == PPB_ERROR_UNSORTED_FIELD_ARR);
    }

    /* Sentinel: all bits values 0-7 (field number 0 is forbidden; bits < 8 triggers the check). */
    for (uint64_t b = 0; b <= 7; b++)
    {
        struct ppb_encoded_tag tags[1] = { { .bits = b } };
        struct ppb_field fields[1];
        zero_fields(1, fields);

        ptrdiff_t ret = ppb_prescan(buf, 1, tags, fields, SIZE_MAX);
        CHECK(ret == PPB_ERROR_SENTINEL_FIELD_ARR);
    }

    /* Empty fields array succeeds on valid data. */
    {
        ptrdiff_t ret = ppb_prescan(buf, 0, NULL, NULL, SIZE_MAX);
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

    struct ppb_encoded_tag tags[2] = { { .bits = 0 }, PPB_TAG(1, PPB_WIRE_VARINT) };
    struct ppb_field fields[2];
    zero_fields(2, fields);
    fields[1].m.num_occurrences = 42; /* canary */

    ptrdiff_t ret = ppb_prescan(buf, 2, tags, fields, SIZE_MAX);
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
        struct ppb_encoded_tag tags[1] = { PPB_TAG(1, PPB_WIRE_VARINT) };
        struct ppb_field fields[1];
        zero_fields(1, fields);

        ptrdiff_t ret = ppb_prescan(buf, 1, tags, fields, SIZE_MAX);
        CHECK(ret == (ptrdiff_t)sizeof(wire));
        CHECK(fields[0].m.num_occurrences == 1);
        CHECK(fields[0].v.u64 == 1);
    }

    /* With catch-all varint: field 99 routed there. */
    {
        struct ppb_encoded_tag tags[2] = { PPB_TAG(1, PPB_WIRE_VARINT), PPB_TAG(-1, PPB_WIRE_VARINT) };
        struct ppb_field fields[2];
        zero_fields(2, fields);

        ptrdiff_t ret = ppb_prescan(buf, 2, tags, fields, SIZE_MAX);
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

    struct ppb_encoded_tag tags[3] = {
        PPB_TAG(1, PPB_WIRE_VARINT),
        PPB_TAG(2, PPB_WIRE_VARINT),
        PPB_TAG(3, PPB_WIRE_VARINT),
    };
    struct ppb_field fields[3];
    zero_fields(3, fields);

    struct ppb_buf buf = make_buf(wire, sizeof(wire));
    ptrdiff_t ret = ppb_prescan(buf, 3, tags, fields, 1);

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

    struct ppb_encoded_tag tags[1] = { PPB_TAG(1, PPB_WIRE_VARINT) };
    struct ppb_field fields[1];
    zero_fields(1, fields);

    ptrdiff_t ret = ppb_prescan(buf, 1, tags, fields, SIZE_MAX);
    CHECK(ret == PPB_ERROR_CORRUPT_TAG);
}

/* 8 continuation bytes → peek_tag fast path, stop_bits==0 → CORRUPT_TAG. */
static void
test_prescan_corrupt_tag(void)
{
    printf("test_prescan_corrupt_tag\n");

    static const uint8_t wire[] = {
        0x80, 0xff, 0xff, 0xff, 0x80, 0x80, 0x80, 0x80, /* overlong tag */
        0x80, 0x80, 0x80, 0x00, /* random varint */
    };
    struct ppb_buf buf = make_buf(wire, sizeof(wire));

    struct ppb_encoded_tag tags[1] = { PPB_TAG(1, PPB_WIRE_VARINT) };
    struct ppb_field fields[1];
    zero_fields(1, fields);

    ptrdiff_t ret = ppb_prescan(buf, 1, tags, fields, SIZE_MAX);
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

    struct ppb_encoded_tag tags[1] = { PPB_TAG(1, PPB_WIRE_VARINT) };
    struct ppb_field fields[1];
    zero_fields(1, fields);

    ptrdiff_t ret = ppb_prescan(buf, 1, tags, fields, SIZE_MAX);
    CHECK(ret == PPB_ERROR_TRUNCATED_DATA);
}

/* All 4 wire types, in ascending field order. */
static void
test_lexn_known_fields_in_order(void)
{
    printf("test_lexn_known_fields_in_order\n");

    struct ppb_encoded_tag tags[4];
    init_four_tags(tags);
    struct ppb_field fields[4];
    init_four_fields(fields);

    struct ppb_buf buf = make_buf(four_field_wire, sizeof(four_field_wire));
    struct ppb_lexn_ret ret = ppb_lexn(&buf, 4, tags, fields, 4);

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

    struct ppb_encoded_tag tags[4];
    init_four_tags(tags);
    struct ppb_field fields[4];
    init_four_fields(fields);

    struct ppb_buf buf = make_buf(four_field_wire, sizeof(four_field_wire));
    struct ppb_lexn_ret ret;

    ret = ppb_lexn(&buf, 4, tags, fields, 1);
    CHECK(ret.status == PPB_OK);
    CHECK(ret.first_field == 0);
    CHECK(ret.field_range == 1);
    CHECK(fields[0].v.u64 == 150);

    ret = ppb_lexn(&buf, 4, tags, fields, 1);
    CHECK(ret.status == PPB_OK);
    CHECK(ret.first_field == 1);
    CHECK(ret.field_range == 1);
    CHECK(fields[1].v.u64 == 1);

    ret = ppb_lexn(&buf, 4, tags, fields, 1);
    CHECK(ret.status == PPB_OK);
    CHECK(ret.first_field == 2);
    CHECK(ret.field_range == 1);
    CHECK(fields[2].v.payload.size == 5);

    ret = ppb_lexn(&buf, 4, tags, fields, 1);
    CHECK(ret.status == PPB_OK);
    CHECK(ret.first_field == 3);
    CHECK(ret.field_range == 1);
    CHECK(fields[3].v.u64 == 42);

    /* EOF */
    ret = ppb_lexn(&buf, 4, tags, fields, 1);
    CHECK(ret.status == PPB_OK);
    CHECK(ret.field_range == 0);
}

/* Make sure we correctly no-op when we want 0 field/ */
static void
test_lexn_no_field(void)
{
    printf("test_lexn_no_field\n");

    struct ppb_buf buf = make_buf(four_field_wire, sizeof(four_field_wire));
    struct ppb_lexn_ret ret = ppb_lexn(&buf, 0, NULL, NULL, 4);

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
    struct ppb_encoded_tag tags[NUM_FIELDS] = {
        PPB_TAG(-1, PPB_WIRE_VARINT),
        PPB_TAG(-1, PPB_WIRE_I64),
        PPB_TAG(-1, PPB_WIRE_LEN),
        PPB_TAG(-1, PPB_WIRE_I32),
    };
    struct ppb_field fields[NUM_FIELDS] = { 0 };

    struct ppb_buf buf = make_buf(four_field_wire, sizeof(four_field_wire));

    {
        struct ppb_lexn_ret ret = ppb_lexn(&buf, NUM_FIELDS, tags, fields, SIZE_MAX);

        CHECK(ret.status == PPB_OK);
        // varint
        CHECK(ret.first_field == 0);
        CHECK(ret.field_range == 1);
        CHECK(fields[0].v.u64 == 150);
    }

    {
        struct ppb_lexn_ret ret = ppb_lexn(&buf, NUM_FIELDS, tags, fields, SIZE_MAX);

        CHECK(ret.status == PPB_OK);
        // i64
        CHECK(ret.first_field == 1);
        CHECK(ret.field_range == 1);
        CHECK(fields[1].v.u64 == 1);
    }

    {
        struct ppb_lexn_ret ret = ppb_lexn(&buf, NUM_FIELDS, tags, fields, SIZE_MAX);

        CHECK(ret.status == PPB_OK);
        // len
        CHECK(ret.first_field == 2);
        CHECK(ret.field_range == 1);
        CHECK(fields[2].v.payload.size == 5);
        CHECK(memcmp(fields[2].v.payload.buf, "hello", 5) == 0);
    }

    {
        struct ppb_lexn_ret ret = ppb_lexn(&buf, NUM_FIELDS, tags, fields, SIZE_MAX);

        CHECK(ret.status == PPB_OK);
        // i32
        CHECK(ret.first_field == 3);
        CHECK(ret.field_range == 1);
        CHECK(fields[3].v.u64 == 42);
    }

    /* EOF */
    {
        struct ppb_lexn_ret ret = ppb_lexn(&buf, NUM_FIELDS, tags, fields, SIZE_MAX);

        CHECK(ret.status == PPB_OK);
        CHECK(ret.first_field == 0);
        CHECK(ret.field_range == 0);
    }

    {
        struct ppb_lexn_ret ret = ppb_lexn(&buf, NUM_FIELDS, tags, fields, SIZE_MAX);

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

    struct ppb_encoded_tag tags[2] = { PPB_TAG(1, PPB_WIRE_VARINT), PPB_TAG(2, PPB_WIRE_VARINT) };
    struct ppb_field fields[2];
    zero_fields(2, fields);

    struct ppb_buf buf = make_buf(wire, sizeof(wire));
    struct ppb_lexn_ret ret;

    ret = ppb_lexn(&buf, 2, tags, fields, 4);
    CHECK(ret.status == PPB_OK);
    CHECK(ret.first_field == 1);
    CHECK(ret.field_range == 1);
    CHECK(fields[1].v.u64 == 2);

    ret = ppb_lexn(&buf, 2, tags, fields, 4);
    CHECK(ret.status == PPB_OK);
    CHECK(ret.first_field == 0);
    CHECK(ret.field_range == 1);
    CHECK(fields[0].v.u64 == 1);

    ret = ppb_lexn(&buf, 2, tags, fields, 4);
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

    struct ppb_encoded_tag tags[2] = { PPB_TAG(1, PPB_WIRE_VARINT), PPB_TAG(3, PPB_WIRE_VARINT) };
    struct ppb_field fields[2];
    zero_fields(2, fields);

    struct ppb_buf buf = make_buf(wire, sizeof(wire));
    struct ppb_lexn_ret ret = ppb_lexn(&buf, 2, tags, fields, 4);

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

    struct ppb_encoded_tag tags[2] = { PPB_TAG(1, PPB_WIRE_VARINT), PPB_TAG(3, PPB_WIRE_VARINT) };
    struct ppb_field fields[2];
    zero_fields(2, fields);

    struct ppb_buf buf = make_buf(wire, sizeof(wire));
    struct ppb_lexn_ret ret = ppb_lexn(&buf, 2, tags, fields, 4);

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

    struct ppb_encoded_tag tags[2] = { PPB_TAG(1, PPB_WIRE_VARINT), PPB_TAG(3, PPB_WIRE_VARINT) };
    struct ppb_field fields[2];
    zero_fields(2, fields);

    struct ppb_buf buf = make_buf(wire, sizeof(wire));
    struct ppb_lexn_ret ret = ppb_lexn(&buf, 2, tags, fields, 4);

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

    struct ppb_encoded_tag tags[4] = {
        PPB_TAG(1, PPB_WIRE_VARINT),
        PPB_TAG(2, PPB_WIRE_VARINT),
        PPB_TAG(3, PPB_WIRE_VARINT),
        PPB_TAG(4, PPB_WIRE_VARINT),
    };
    struct ppb_field fields[4];
    zero_fields(4, fields);

    struct ppb_buf buf = make_buf(wire, sizeof(wire));
    struct ppb_lexn_ret ret;

    ret = ppb_lexn(&buf, 4, tags, fields, 4);
    CHECK(ret.status == PPB_OK);
    CHECK(ret.first_field == 0);
    CHECK(ret.field_range == 3); /* idx 0..2 */
    CHECK(fields[0].v.u64 == 10);
    CHECK(fields[2].v.u64 == 30);

    ret = ppb_lexn(&buf, 4, tags, fields, 4);
    CHECK(ret.status == PPB_OK);
    CHECK(ret.first_field == 1);
    CHECK(ret.field_range == 3); /* idx 1..3 */
    CHECK(fields[1].v.u64 == 20);
    CHECK(fields[3].v.u64 == 40);
    CHECK(buf.size == 0);
}

/*
 * Confirm that we lex catch-all fields one by one, even when they
 * have different type tags.
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

    struct ppb_encoded_tag tags[3] = {
        PPB_TAG(1, PPB_WIRE_VARINT),
        PPB_TAG(-1, PPB_WIRE_VARINT),
        PPB_TAG(-1, PPB_WIRE_I32),
    };
    struct ppb_field fields[3];
    zero_fields(3, fields);

    struct ppb_buf buf = make_buf(wire, sizeof(wire));
    struct ppb_lexn_ret ret;

    ret = ppb_lexn(&buf, 3, tags, fields, 4);
    CHECK(ret.status == PPB_OK);
    CHECK(ret.first_field == 0);
    CHECK(ret.field_range == 2);
    CHECK(fields[0].v.u64 == 42);
    CHECK(fields[1].v.u64 == 7);

    ret = ppb_lexn(&buf, 3, tags, fields, 4);
    CHECK(ret.status == PPB_OK);
    CHECK(ret.first_field == 2);
    CHECK(ret.field_range == 1);
    CHECK(fields[2].v.u64 == 1);

    ret = ppb_lexn(&buf, 3, tags, fields, 4);
    CHECK(ret.field_range == 0);
}

/*
 * Like test_lexn_catchall_always_stops, but with descending wire types:
 * an unknown i32 (wire=5) followed by an unknown varint (wire=0).
 */
static void
test_lexn_catchall_always_stops_descending(void)
{
    printf("test_lexn_catchall_always_stops_descending\n");

    static const uint8_t wire[] = {
        0x9d, 0x06, 0x01, 0x00, 0x00, 0x00,  /* field 99 i32 1 → catch-all i32 */
        0xa0, 0x06, 0x02,                    /* field 100 varint 2 → catch-all varint */
    };

    /* sorted ascending by PPB_TAG_BITS(-1, wire_type) */
    struct ppb_encoded_tag tags[2] = {
        PPB_TAG(-1, PPB_WIRE_VARINT),
        PPB_TAG(-1, PPB_WIRE_I32),
    };
    struct ppb_field fields[2];
    zero_fields(2, fields);

    struct ppb_buf buf = make_buf(wire, sizeof(wire));
    struct ppb_lexn_ret ret;

    ret = ppb_lexn(&buf, 2, tags, fields, 4);
    CHECK(ret.status == PPB_OK);
    CHECK(ret.first_field == 1);  /* catch-all i32 */
    CHECK(ret.field_range == 1);
    CHECK(fields[1].v.u32 == 1);

    ret = ppb_lexn(&buf, 2, tags, fields, 4);
    CHECK(ret.status == PPB_OK);
    CHECK(ret.first_field == 0);  /* catch-all varint */
    CHECK(ret.field_range == 1);
    CHECK(fields[0].v.u64 == 2);

    ret = ppb_lexn(&buf, 2, tags, fields, 4);
    CHECK(ret.field_range == 0);
}

static void
test_lexn_empty(void)
{
    printf("test_lexn_empty\n");

    struct ppb_encoded_tag tags[1] = { PPB_TAG(1, PPB_WIRE_VARINT) };
    struct ppb_field fields[1];
    zero_fields(1, fields);

    struct ppb_buf buf = make_buf("", 0);
    struct ppb_lexn_ret ret = ppb_lexn(&buf, 1, tags, fields, 4);

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

    struct ppb_encoded_tag tags[1] = { PPB_TAG(1, PPB_WIRE_VARINT) };
    struct ppb_field fields[1];
    zero_fields(1, fields);

    struct ppb_buf buf = make_buf(wire, sizeof(wire));
    struct ppb_lexn_ret ret = ppb_lexn(&buf, 1, tags, fields, 4);

    CHECK(ret.status == PPB_ERROR_TRUNCATED_DATA);
}

/*
 * For each of the 8 wire types, verify two things:
 *   1. Field index 1 with that wire type: succeeds for the four valid wire
 *      types (VARINT, I64, LEN, I32), returns CORRUPT_TAG for the four
 *      invalid ones (SGROUP, EGROUP, and the two reserved types).
 *   2. Flipping bit 3 of the tag byte to set field index 0: must always
 *      return CORRUPT_TAG regardless of wire type, because field numbers
 *      start at 1.
 *
 * Plus, smoke test the similar behaviour for _prescan while we're here.
 */

static void
test_lexn_field_zero_wire0(void)
{
    printf("test_lexn_field_zero_wire0 (VARINT)\n");

    struct ppb_encoded_tag tags[1] = { PPB_TAG(1, PPB_WIRE_VARINT) };
    struct ppb_field fields[1];

    /* Field 1, varint: succeeds. */
    {
        static const uint8_t wire[] = { 0x08, 0x01 };
        struct ppb_buf buf = make_buf(wire, sizeof(wire));
        zero_fields(1, fields);

        CHECK(ppb_prescan(buf, 1, tags, fields, 4) == sizeof(wire));

        struct ppb_lexn_ret ret = ppb_lexn(&buf, 1, tags, fields, 4);
        CHECK(ret.status == PPB_OK);
        CHECK(ret.field_range == 1);
        CHECK(fields[0].v.u64 == 1);
    }

    /* Field 0, varint (tag byte 0x00): CORRUPT_TAG. */
    {
        static const uint8_t wire[] = { 0x00, 0x01 };
        struct ppb_buf buf = make_buf(wire, sizeof(wire));
        zero_fields(1, fields);

        CHECK(ppb_prescan(buf, 1, tags, fields, 4) == PPB_ERROR_CORRUPT_TAG);

        struct ppb_lexn_ret ret = ppb_lexn(&buf, 1, tags, fields, 4);
        CHECK(ret.status == PPB_ERROR_CORRUPT_TAG);
        CHECK(ret.field_range == 0);
    }
}

static void
test_lexn_field_zero_wire1(void)
{
    printf("test_lexn_field_zero_wire1 (I64)\n");

    struct ppb_encoded_tag tags[1] = { PPB_TAG(1, PPB_WIRE_I64) };
    struct ppb_field fields[1];

    /* Field 1, I64: succeeds. */
    {
        static const uint8_t wire[] = { 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
        struct ppb_buf buf = make_buf(wire, sizeof(wire));
        zero_fields(1, fields);

        CHECK(ppb_prescan(buf, 1, tags, fields, 4) == sizeof(wire));

        struct ppb_lexn_ret ret = ppb_lexn(&buf, 1, tags, fields, 4);
        CHECK(ret.status == PPB_OK);
        CHECK(ret.field_range == 1);
        CHECK(fields[0].v.u64 == 0);
    }

    /* Field 0, I64 (tag byte 0x01): CORRUPT_TAG. */
    {
        static const uint8_t wire[] = { 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
        struct ppb_buf buf = make_buf(wire, sizeof(wire));
        zero_fields(1, fields);

        CHECK(ppb_prescan(buf, 1, tags, fields, 4) == PPB_ERROR_CORRUPT_TAG);

        struct ppb_lexn_ret ret = ppb_lexn(&buf, 1, tags, fields, 4);
        CHECK(ret.status == PPB_ERROR_CORRUPT_TAG);
        CHECK(ret.field_range == 0);
    }
}

static void
test_lexn_field_zero_wire2(void)
{
    printf("test_lexn_field_zero_wire2 (LEN)\n");

    struct ppb_encoded_tag tags[1] = { PPB_TAG(1, PPB_WIRE_LEN) };
    struct ppb_field fields[1];

    /* Field 1, LEN, length 1: succeeds. */
    {
        static const uint8_t wire[] = { 0x0A, 0x01, 0x42 };
        struct ppb_buf buf = make_buf(wire, sizeof(wire));
        zero_fields(1, fields);

        CHECK(ppb_prescan(buf, 1, tags, fields, 4) == sizeof(wire));

        struct ppb_lexn_ret ret = ppb_lexn(&buf, 1, tags, fields, 4);
        CHECK(ret.status == PPB_OK);
        CHECK(ret.field_range == 1);
        CHECK(fields[0].v.payload.size == 1);
    }

    /* Field 0, LEN (tag byte 0x02): CORRUPT_TAG. */
    {
        static const uint8_t wire[] = { 0x02, 0x01, 0x42 };
        struct ppb_buf buf = make_buf(wire, sizeof(wire));
        zero_fields(1, fields);

        CHECK(ppb_prescan(buf, 1, tags, fields, 4) == PPB_ERROR_CORRUPT_TAG);

        struct ppb_lexn_ret ret = ppb_lexn(&buf, 1, tags, fields, 4);
        CHECK(ret.status == PPB_ERROR_CORRUPT_TAG);
        CHECK(ret.field_range == 0);
    }
}

static void
test_lexn_field_zero_wire3(void)
{
    printf("test_lexn_field_zero_wire3 (SGROUP, invalid)\n");

    /* Wire type 3 (SGROUP) is rejected by handle_field even for field 1. */
    struct ppb_encoded_tag tags[1] = { { .bits = PPB_TAG_BITS(1, 3) } };
    struct ppb_field fields[1];

    /* Field 1, SGROUP: CORRUPT_TAG (invalid wire type); tag was matched so field_range=1. */
    {
        static const uint8_t wire[] = { 0x0B };
        struct ppb_buf buf = make_buf(wire, sizeof(wire));
        zero_fields(1, fields);

        CHECK(ppb_prescan(buf, 1, tags, fields, 4) == PPB_ERROR_CORRUPT_TAG);

        struct ppb_lexn_ret ret = ppb_lexn(&buf, 1, tags, fields, 4);
        CHECK(ret.status == PPB_ERROR_CORRUPT_TAG);
        CHECK(ret.field_range == 1);
    }

    /* Field 0, SGROUP (tag byte 0x03): CORRUPT_TAG. */
    {
        static const uint8_t wire[] = { 0x03 };
        struct ppb_buf buf = make_buf(wire, sizeof(wire));
        zero_fields(1, fields);

        CHECK(ppb_prescan(buf, 1, tags, fields, 4) == PPB_ERROR_CORRUPT_TAG);

        struct ppb_lexn_ret ret = ppb_lexn(&buf, 1, tags, fields, 4);
        CHECK(ret.status == PPB_ERROR_CORRUPT_TAG);
        CHECK(ret.field_range == 0);
    }
}

static void
test_lexn_field_zero_wire4(void)
{
    printf("test_lexn_field_zero_wire4 (EGROUP, invalid)\n");

    /* Wire type 4 (EGROUP) is rejected by handle_field even for field 1. */
    struct ppb_encoded_tag tags[1] = { { .bits = PPB_TAG_BITS(1, 4) } };
    struct ppb_field fields[1];

    /* Field 1, EGROUP: CORRUPT_TAG (invalid wire type); tag was matched so field_range=1. */
    {
        static const uint8_t wire[] = { 0x0C };
        struct ppb_buf buf = make_buf(wire, sizeof(wire));
        zero_fields(1, fields);

        CHECK(ppb_prescan(buf, 1, tags, fields, 4) == PPB_ERROR_CORRUPT_TAG);

        struct ppb_lexn_ret ret = ppb_lexn(&buf, 1, tags, fields, 4);
        CHECK(ret.status == PPB_ERROR_CORRUPT_TAG);
        CHECK(ret.field_range == 1);
    }

    /* Field 0, EGROUP (tag byte 0x04): CORRUPT_TAG. */
    {
        static const uint8_t wire[] = { 0x04 };
        struct ppb_buf buf = make_buf(wire, sizeof(wire));
        zero_fields(1, fields);

        CHECK(ppb_prescan(buf, 1, tags, fields, 4) == PPB_ERROR_CORRUPT_TAG);

        struct ppb_lexn_ret ret = ppb_lexn(&buf, 1, tags, fields, 4);
        CHECK(ret.status == PPB_ERROR_CORRUPT_TAG);
        CHECK(ret.field_range == 0);
    }
}

static void
test_lexn_field_zero_wire5(void)
{
    printf("test_lexn_field_zero_wire5 (I32)\n");

    struct ppb_encoded_tag tags[1] = { PPB_TAG(1, PPB_WIRE_I32) };
    struct ppb_field fields[1];

    /* Field 1, I32: succeeds. */
    {
        static const uint8_t wire[] = { 0x0D, 0x00, 0x00, 0x00, 0x00 };
        struct ppb_buf buf = make_buf(wire, sizeof(wire));
        zero_fields(1, fields);

        CHECK(ppb_prescan(buf, 1, tags, fields, 4) == sizeof(wire));

        struct ppb_lexn_ret ret = ppb_lexn(&buf, 1, tags, fields, 4);
        CHECK(ret.status == PPB_OK);
        CHECK(ret.field_range == 1);
        CHECK(fields[0].v.u32 == 0);
    }

    /* Field 0, I32 (tag byte 0x05): CORRUPT_TAG. */
    {
        static const uint8_t wire[] = { 0x05, 0x00, 0x00, 0x00, 0x00 };
        struct ppb_buf buf = make_buf(wire, sizeof(wire));
        zero_fields(1, fields);

        CHECK(ppb_prescan(buf, 1, tags, fields, 4) == PPB_ERROR_CORRUPT_TAG);

        struct ppb_lexn_ret ret = ppb_lexn(&buf, 1, tags, fields, 4);
        CHECK(ret.status == PPB_ERROR_CORRUPT_TAG);
        CHECK(ret.field_range == 0);
    }
}

static void
test_lexn_field_zero_wire6(void)
{
    printf("test_lexn_field_zero_wire6 (reserved, invalid)\n");

    /* Wire type 6 is reserved and rejected by handle_field even for field 1. */
    struct ppb_encoded_tag tags[1] = { { .bits = PPB_TAG_BITS(1, 6) } };
    struct ppb_field fields[1];

    /* Field 1, wire 6: CORRUPT_TAG (invalid wire type); tag was matched so field_range=1. */
    {
        static const uint8_t wire[] = { 0x0E };
        struct ppb_buf buf = make_buf(wire, sizeof(wire));
        zero_fields(1, fields);

        CHECK(ppb_prescan(buf, 1, tags, fields, 4) == PPB_ERROR_CORRUPT_TAG);

        struct ppb_lexn_ret ret = ppb_lexn(&buf, 1, tags, fields, 4);
        CHECK(ret.status == PPB_ERROR_CORRUPT_TAG);
        CHECK(ret.field_range == 1);
    }

    /* Field 0, wire 6 (tag byte 0x06): CORRUPT_TAG. */
    {
        static const uint8_t wire[] = { 0x06 };
        struct ppb_buf buf = make_buf(wire, sizeof(wire));
        zero_fields(1, fields);

        CHECK(ppb_prescan(buf, 1, tags, fields, 4) == PPB_ERROR_CORRUPT_TAG);

        struct ppb_lexn_ret ret = ppb_lexn(&buf, 1, tags, fields, 4);
        CHECK(ret.status == PPB_ERROR_CORRUPT_TAG);
        CHECK(ret.field_range == 0);
    }
}

static void
test_lexn_field_zero_wire7(void)
{
    printf("test_lexn_field_zero_wire7 (reserved, invalid)\n");

    /* Wire type 7 is reserved and rejected by handle_field even for field 1. */
    struct ppb_encoded_tag tags[1] = { { .bits = PPB_TAG_BITS(1, 7) } };
    struct ppb_field fields[1];

    /* Field 1, wire 7: CORRUPT_TAG (invalid wire type); tag was matched so field_range=1. */
    {
        static const uint8_t wire[] = { 0x0F };
        struct ppb_buf buf = make_buf(wire, sizeof(wire));
        zero_fields(1, fields);

        CHECK(ppb_prescan(buf, 1, tags, fields, 4) == PPB_ERROR_CORRUPT_TAG);

        struct ppb_lexn_ret ret = ppb_lexn(&buf, 1, tags, fields, 4);
        CHECK(ret.status == PPB_ERROR_CORRUPT_TAG);
        CHECK(ret.field_range == 1);
    }

    /* Field 0, wire 7 (tag byte 0x07): CORRUPT_TAG. */
    {
        static const uint8_t wire[] = { 0x07 };
        struct ppb_buf buf = make_buf(wire, sizeof(wire));
        zero_fields(1, fields);

        CHECK(ppb_prescan(buf, 1, tags, fields, 4) == PPB_ERROR_CORRUPT_TAG);

        struct ppb_lexn_ret ret = ppb_lexn(&buf, 1, tags, fields, 4);
        CHECK(ret.status == PPB_ERROR_CORRUPT_TAG);
        CHECK(ret.field_range == 0);
    }
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

    struct ppb_encoded_tag tags[1] = { PPB_TAG(1, PPB_WIRE_VARINT) };
    struct ppb_field fields[1];
    zero_fields(1, fields);

    struct ppb_lexn_ret ret = ppb_lexn(&buf, 1, tags, fields, 4);
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

    struct ppb_encoded_tag tags[1] = { PPB_TAG(1, PPB_WIRE_VARINT) };
    struct ppb_field fields[1];
    zero_fields(1, fields);

    struct ppb_lexn_ret ret = ppb_lexn(&buf, 1, tags, fields, 4);
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

    struct ppb_encoded_tag tags[1] = { PPB_TAG(1, PPB_WIRE_VARINT) };
    struct ppb_field fields[1];
    zero_fields(1, fields);

    struct ppb_lexn_ret ret = ppb_lexn(&buf, 1, tags, fields, 4);
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

    struct ppb_encoded_tag tags[1] = { PPB_TAG(1, PPB_WIRE_VARINT) };
    struct ppb_field fields[1];
    zero_fields(1, fields);

    struct ppb_buf buf = make_buf(wire, sizeof(wire));
    struct ppb_lexn_ret ret;

    ret = ppb_lexn(&buf, 1, tags, fields, 4);
    CHECK(ret.status == PPB_ERROR_CORRUPT_TAG);
    CHECK(ret.first_field == 0);
    CHECK(ret.field_range == 1);
    CHECK(fields[0].v.u64 == 1);
    CHECK(buf.size == 1);

    ret = ppb_lexn(&buf, 1, tags, fields, 4);
    CHECK(ret.status == PPB_ERROR_CORRUPT_TAG);
    CHECK(ret.field_range == 0);
}

/*
 * Limit tests use four_field_wire (24 bytes total):
 *   bytes  0- 2: field 1 varint 150   (3 bytes)
 *   bytes  3-11: field 2 i64 1        (9 bytes)
 *   bytes 12-18: field 3 LEN "hello"  (7 bytes)
 *   bytes 19-23: field 4 i32 42       (5 bytes)
 *
 * Interesting boundaries:
 *   limit= 3: exact end of field 1
 *   limit= 5: mid-field-2 (starts at 3, ends at 11)
 *   limit=12: exact end of field 2
 *   limit=24: full buffer (same as no limit)
 */

static void
test_prescan_hard_limit(void)
{
    printf("test_prescan_hard_limit\n");

    struct ppb_encoded_tag tags[4];
    init_four_tags(tags);
    struct ppb_field fields[4];

    struct ppb_buf buf = make_buf(four_field_wire, sizeof(four_field_wire));

    /* Limit exactly at a field boundary: returns limit bytes, no error. */
    init_four_fields(fields);
    ptrdiff_t ret = ppb_prescan_with_hard_limit(buf, 3, 4, tags, fields, SIZE_MAX);
    CHECK(ret == 3);
    CHECK(fields[0].m.num_occurrences == 1);
    CHECK(fields[0].v.u64 == 150);
    CHECK(fields[1].m.num_occurrences == 0);

    /* Limit in the middle of field 2: returns LIMIT_EXCEEDED. */
    init_four_fields(fields);
    ret = ppb_prescan_with_hard_limit(buf, 5, 4, tags, fields, SIZE_MAX);
    CHECK(ret == PPB_ERROR_LIMIT_EXCEEDED);

    /* Limit exactly at the end of field 2. */
    init_four_fields(fields);
    ret = ppb_prescan_with_hard_limit(buf, 12, 4, tags, fields, SIZE_MAX);
    CHECK(ret == 12);
    CHECK(fields[0].m.num_occurrences == 1);
    CHECK(fields[1].m.num_occurrences == 1);
    CHECK(fields[2].m.num_occurrences == 0);

    /* Limit larger than buffer: same as no limit. */
    init_four_fields(fields);
    ret = ppb_prescan_with_hard_limit(buf, SIZE_MAX, 4, tags, fields, SIZE_MAX);
    CHECK(ret == 24);
    CHECK(fields[0].m.num_occurrences == 1);
    CHECK(fields[3].m.num_occurrences == 1);

    /* Limit = 0: returns 0 immediately (nothing consumed). */
    init_four_fields(fields);
    ret = ppb_prescan_with_hard_limit(buf, 0, 4, tags, fields, SIZE_MAX);
    CHECK(ret == 0);
    CHECK(fields[0].m.num_occurrences == 0);
}

static void
test_prescan_soft_limit(void)
{
    printf("test_prescan_soft_limit\n");

    struct ppb_encoded_tag tags[4];
    init_four_tags(tags);
    struct ppb_field fields[4];

    struct ppb_buf buf = make_buf(four_field_wire, sizeof(four_field_wire));

    /* Limit exactly at a field boundary: returns limit bytes, no error. */
    init_four_fields(fields);
    ptrdiff_t ret = ppb_prescan_with_soft_limit(buf, 3, 4, tags, fields, SIZE_MAX);
    CHECK(ret == 3);
    CHECK(fields[0].m.num_occurrences == 1);

    /* Limit in the middle of field 2: returns bytes consumed (> limit), no error. */
    init_four_fields(fields);
    ret = ppb_prescan_with_soft_limit(buf, 5, 4, tags, fields, SIZE_MAX);
    CHECK(ret == 12); /* consumed through end of field 2 */
    CHECK(fields[0].m.num_occurrences == 1);
    CHECK(fields[1].m.num_occurrences == 1);
    CHECK(fields[2].m.num_occurrences == 0);

    /* Limit larger than buffer: same as no limit. */
    init_four_fields(fields);
    ret = ppb_prescan_with_soft_limit(buf, SIZE_MAX, 4, tags, fields, SIZE_MAX);
    CHECK(ret == 24);
}

static void
test_lexn_hard_limit(void)
{
    printf("test_lexn_hard_limit\n");

    struct ppb_encoded_tag tags[4];
    init_four_tags(tags);
    struct ppb_field fields[4];

    /* Limit exactly at a field boundary: field decoded, buf advanced by limit. */
    {
        init_four_fields(fields);
        struct ppb_buf buf = make_buf(four_field_wire, sizeof(four_field_wire));
        struct ppb_lexn_ret ret = ppb_lexn_with_hard_limit(&buf, 3, 4, tags, fields, SIZE_MAX);
        CHECK(ret.status == PPB_OK);
        CHECK(ret.first_field == 0);
        CHECK(ret.field_range == 1);
        CHECK(fields[0].v.u64 == 150);
        CHECK(buf.size == 21); /* 24 - 3 */
    }

    /* Limit mid-field-2: LIMIT_EXCEEDED; field 1 (boundary) + field 2 (overshoot) consumed. */
    {
        init_four_fields(fields);
        struct ppb_buf buf = make_buf(four_field_wire, sizeof(four_field_wire));
        struct ppb_lexn_ret ret = ppb_lexn_with_hard_limit(&buf, 5, 4, tags, fields, SIZE_MAX);
        CHECK(ret.status == PPB_ERROR_LIMIT_EXCEEDED);
        /* Fields 1 and 2 were decoded before the error was detected. */
        CHECK(fields[0].v.u64 == 150);
        CHECK(fields[1].v.u64 == 1);
        CHECK(buf.size == 12); /* 24 - 12: stopped after field 2 */
    }

    /* Limit larger than buffer: same as no limit. */
    {
        init_four_fields(fields);
        struct ppb_buf buf = make_buf(four_field_wire, sizeof(four_field_wire));
        struct ppb_lexn_ret ret = ppb_lexn_with_hard_limit(&buf, SIZE_MAX, 4, tags, fields, SIZE_MAX);
        CHECK(ret.status == PPB_OK);
        CHECK(ret.field_range == 4);
        CHECK(buf.size == 0);
    }
}

static void
test_lexn_soft_limit(void)
{
    printf("test_lexn_soft_limit\n");

    struct ppb_encoded_tag tags[4];
    init_four_tags(tags);
    struct ppb_field fields[4];

    /* Limit mid-field-2: field 2 fully decoded (no error). */
    init_four_fields(fields);
    struct ppb_buf buf = make_buf(four_field_wire, sizeof(four_field_wire));
    struct ppb_lexn_ret ret = ppb_lexn_with_soft_limit(&buf, 5, 4, tags, fields, SIZE_MAX);
    CHECK(ret.status == PPB_OK);
    CHECK(fields[0].v.u64 == 150);
    CHECK(fields[1].v.u64 == 1);
    CHECK(buf.size == 12); /* consumed through end of field 2 */
}

static void
test_tag_macros(void)
{
    printf("test_tag_macros\n");

    {
        struct ppb_encoded_tag t = PPB_TAG(1, PPB_WIRE_VARINT);
        CHECK(t.bits == 0x08);
    }
    {
        struct ppb_encoded_tag t = PPB_TAG(1, PPB_WIRE_I64);
        CHECK(t.bits == 0x09);
    }
    {
        struct ppb_encoded_tag t = PPB_TAG(1, PPB_WIRE_LEN);
        CHECK(t.bits == 0x0A);
    }
    {
        struct ppb_encoded_tag t = PPB_TAG(2, PPB_WIRE_VARINT);
        CHECK(t.bits == 0x10);
    }
    {
        struct ppb_encoded_tag t = PPB_TAG(15, PPB_WIRE_VARINT);
        CHECK(t.bits == 0x78);
    }
    {
        struct ppb_encoded_tag t = PPB_TAG(16, PPB_WIRE_VARINT);
        CHECK(t.bits == 0x0180);
    }
    {
        struct ppb_encoded_tag t = PPB_TAG(-1, PPB_WIRE_VARINT);
        CHECK(t.bits == ((UINT64_MAX << 3) | 0));
    }
    {
        struct ppb_encoded_tag t = PPB_TAG(-1, PPB_WIRE_I32);
        CHECK(t.bits == ((UINT64_MAX << 3) | 5));
    }
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
    test_peekn();
    test_decode_varint();
    test_peek_varint_error();
    test_peek_tag_error();
    test_peek_tag_range();

    test_prescan_known_fields();
    test_prescan_repeated_fields();
    test_prescan_repeated_len();
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
    test_lexn_catchall_always_stops_descending();
    test_lexn_empty();
    test_lexn_error();
    test_lexn_field_zero_wire0();
    test_lexn_field_zero_wire1();
    test_lexn_field_zero_wire2();
    test_lexn_field_zero_wire3();
    test_lexn_field_zero_wire4();
    test_lexn_field_zero_wire5();
    test_lexn_field_zero_wire6();
    test_lexn_field_zero_wire7();
    test_lexn_zero_tag();
    test_lexn_corrupt_tag();
    test_lexn_truncated_tag();
    test_lexn_zero_tag_after_valid();

    test_prescan_hard_limit();
    test_prescan_soft_limit();
    test_lexn_hard_limit();
    test_lexn_soft_limit();

    test_tag_macros();
    test_tag_varint_roundtrip();

    printf("\n%d checks, %d failures\n", g_check_count, g_fail_count);
    return g_fail_count > 0 ? 1 : 0;
}
