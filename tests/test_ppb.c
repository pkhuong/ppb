/*
 * Unit tests for ppb internals: ppb_zag, ppb_decode_varint,
 * ppb_prescan, ppb_lexn, and PPB_TAG macros.
 *
 * Build: gcc -std=c2x -O2 -Iinclude/ -Wall -Wextra -Wpedantic -o build/test_ppb tests/test_ppb.c src/ppb.c
 * Run:   build/test_ppb
 */
#include "ppb/ppb.h"
#include "src/sat_sub.h"
#include "src/varint.h"

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
test_zag32(void)
{
    printf("test_zag32\n");

    /* Canonical sint32 encodings decode exactly like ppb_zag. */
    CHECK(ppb_zag32(0) == 0);
    CHECK(ppb_zag32(1) == -1);
    CHECK(ppb_zag32(2) == 1);
    CHECK(ppb_zag32(3) == -2);
    CHECK(ppb_zag32(4) == 2);
    CHECK(ppb_zag32(UINT32_MAX - 1) == INT32_MAX);
    CHECK(ppb_zag32(UINT32_MAX) == INT32_MIN);

    /*
     * Non-canonical sint32: a value with bits set above bit 32 (e.g. a
     * 10-byte varint on the wire).  ppb_zag decodes the full 64-bit
     * value, while ppb_zag32 truncates to 32 bits first to match
     * google's C++ parser, so the two disagree.
     */
    {
        uint64_t noncanonical = ((uint64_t)1 << 32) | 1;

        CHECK(ppb_zag32((uint32_t)noncanonical) == -1);
        CHECK(ppb_zag(noncanonical) == -2147483649LL);
        CHECK((int64_t)ppb_zag32((uint32_t)noncanonical) != ppb_zag(noncanonical));
    }
}

static void
test_saturating_subzu(void)
{
    printf("test_saturating_subzu\n");

    /* Normal behavior: x >= y => x - y. */
    CHECK(saturating_subzu(0, 0) == 0);
    CHECK(saturating_subzu(1, 0) == 1);
    CHECK(saturating_subzu(1, 1) == 0);
    CHECK(saturating_subzu(42, 17) == 25);
    CHECK(saturating_subzu(SIZE_MAX, 0) == SIZE_MAX);
    CHECK(saturating_subzu(SIZE_MAX, 1) == SIZE_MAX - 1);
    CHECK(saturating_subzu(SIZE_MAX, SIZE_MAX) == 0);

    /* Saturating behavior: x < y => 0. */
    CHECK(saturating_subzu(0, 1) == 0);
    CHECK(saturating_subzu(0, SIZE_MAX) == 0);
    CHECK(saturating_subzu(1, 2) == 0);
    CHECK(saturating_subzu(17, 42) == 0);
    CHECK(saturating_subzu(SIZE_MAX - 1, SIZE_MAX) == 0);
}

static void
test_saturating_range_u32(void)
{
    printf("test_saturating_range_u32\n");

    /* In range: x == y => 1. */
    CHECK(saturating_range_u32(0, 0) == 1);
    CHECK(saturating_range_u32(1, 1) == 1);
    CHECK(saturating_range_u32(SIZE_MAX, SIZE_MAX) == 1);

    /* In range: small deltas. */
    CHECK(saturating_range_u32(0, 1) == 2);
    CHECK(saturating_range_u32(10, 19) == 10);
    CHECK(saturating_range_u32(100, 1000) == 901);

    /* In range, upper boundary: y - x == UINT32_MAX - 1 => 1 + y - x == UINT32_MAX. */
    CHECK(saturating_range_u32(0, (size_t)UINT32_MAX - 1) == UINT32_MAX);
    CHECK(saturating_range_u32(7, (size_t)UINT32_MAX - 1 + 7) == UINT32_MAX);

    /* Saturate boundary: y - x == UINT32_MAX => UINT32_MAX. */
    CHECK(saturating_range_u32(0, (size_t)UINT32_MAX) == UINT32_MAX);
    CHECK(saturating_range_u32(1, (size_t)UINT32_MAX + 1) == UINT32_MAX);

    /* Saturate: large deltas. */
    if (SIZE_MAX > UINT32_MAX)
    {
        CHECK(saturating_range_u32(0, SIZE_MAX) == UINT32_MAX);
        CHECK(saturating_range_u32(SIZE_MAX / 2, SIZE_MAX) == UINT32_MAX);
    }
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

/*
 * Pin 10-byte varint truncation: only bit 0 of the 10th byte contributes
 * (bits 1-6 would encode bits 64-69, which overflow uint64_t and are lost).
 */
static void
test_decode_varint_10byte(void)
{
    printf("test_decode_varint_10byte\n");

    /* UINT64_MAX, drop extra bits. */
    {
        uint8_t data[] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f };
        struct ppb_buf buf = make_buf(data, sizeof(data));
        enum ppb_error err = PPB_OK;
        uint64_t val = ppb_decode_varint(&buf, &err);
        CHECK(err == PPB_OK);
        CHECK(val == UINT64_MAX);
        CHECK(buf.size == 0);
    }

    /* UINT64_MAX, only one extra bit. */
    {
        uint8_t data[] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x03 };
        struct ppb_buf buf = make_buf(data, sizeof(data));
        enum ppb_error err = PPB_OK;
        uint64_t val = ppb_decode_varint(&buf, &err);
        CHECK(err == PPB_OK);
        CHECK(val == UINT64_MAX);
        CHECK(buf.size == 0);
    }

    /* Last byte is a stop bit, and has the low bit clear -> top bit of uint64_t is zero. */
    {
        uint8_t data[] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7e };
        struct ppb_buf buf = make_buf(data, sizeof(data));
        enum ppb_error err = PPB_OK;
        uint64_t val = ppb_decode_varint(&buf, &err);
        CHECK(err == PPB_OK);
        CHECK(val == UINT64_MAX >> 1);
        CHECK(buf.size == 0);
    }

    /* Continuation bytes all zero, last byte is 1 -> only top bit is set. */
    {
        uint8_t data[] = { 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01 };
        struct ppb_buf buf = make_buf(data, sizeof(data));
        enum ppb_error err = PPB_OK;
        uint64_t val = ppb_decode_varint(&buf, &err);
        CHECK(err == PPB_OK);
        CHECK(val == (uint64_t)1 << 63);
        CHECK(buf.size == 0);
    }

    /* Same thing, but now the low bit of the last byte is 0 -> result is 0. */
    {
        uint8_t data[] = { 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x7e };
        struct ppb_buf buf = make_buf(data, sizeof(data));
        enum ppb_error err = PPB_OK;
        uint64_t val = ppb_decode_varint(&buf, &err);
        CHECK(err == PPB_OK);
        CHECK(val == 0);
        CHECK(buf.size == 0);
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
 * Slow path: peek_varint_slow with limb_width=8 loops while shift < 64, so it
 * accepts at most 8 bytes.  Through the public peek_tag it only runs when
 * src.size < 8 (<= 7 bytes, max value 2^55-1); but even when called directly
 * it can only return a value < 2^63.
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
     * Exercise the peek_tag slow path with src.size < 8.
     */
    for (size_t n = 2; n <= 7; n++)
    {
        uint8_t data[7];
        for (size_t i = 0; i + 1 < n; i++)
            data[i] = 0xFF;
        data[n - 1] = 0x7F;

        uint64_t val = 0;
        int rc = peek_tag(make_buf(data, n), &val);
        CHECK(rc == (int)n);
        CHECK(val == (((uint64_t)1 << (8 * n - 1)) - 1));
        CHECK(val < ((uint64_t)1 << 63));
    }

    /*
     * Also exercise the slow path directly, with an 8-byte input.
     */
    {
        uint8_t data[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F };
        uint64_t val = 0;
        int rc = peek_varint_slow(make_buf(data, sizeof(data)), &val, /*limb_width=*/8,
            PPB_ERROR_CORRUPT_TAG);
        CHECK(rc == 8);
        CHECK(val == (UINT64_MAX >> 1));  /* 2^63 - 1 */
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

/*
 * Over-read at the varint fast<->slow boundary and at per-wire-type EOF.
 * Every buffer is heap-allocated at exactly its length, so a read at offset
 * `size` (one past the end) trips an ASan red-zone; the peek tests above use
 * oversized stack arrays where the same over-read would land inside the array
 * and escape the sanitizer.
 */

/* Prescan every truncation of `msg` against an exact-sized heap copy. */
static void
overread_truncation_sweep(const uint8_t *msg, size_t len, size_t num_fields,
    const struct ppb_encoded_tag *tags)
{
    CHECK(ppb_validate_tags(num_fields, tags) == PPB_OK);

    for (size_t cut = 0; cut <= len; cut++)
    {
        uint8_t *p = malloc(cut == 0 ? 1 : cut);
        memcpy(p, msg, cut);

        struct ppb_field fields[2];
        zero_fields(num_fields, fields);

        ptrdiff_t rc = ppb_prescan(make_buf(p, cut), num_fields, tags, fields, SIZE_MAX);
        CHECK(rc < 0 || (size_t)rc <= cut); /* never consumes past the buffer */

        free(p);
    }
}

static void
test_overread_boundary_exact(void)
{
    printf("test_overread_boundary_exact\n");

    /*
     * Direct peek sweep across the fast/slow cutoff (size 7/8/9) on
     * exact-sized buffers.  All-0xFF forces the fast path to find no stop bit
     * and fall into the byte-by-byte slow path; clearing the high bit of the
     * last byte makes a valid varint that ends exactly at EOF.
     */
    for (size_t cut = 0; cut <= 12; cut++)
    {
        uint8_t *p = malloc(cut == 0 ? 1 : cut);

        memset(p, 0xFF, cut); /* all continuation bytes */

        uint64_t v = 0xDEAD;
        int rc = peek_varint(make_buf(p, cut), &v);
        CHECK(rc < 0 || (size_t)rc <= cut);

        uint64_t t = 0xBEEF;
        int rt = peek_tag(make_buf(p, cut), &t);
        CHECK(rt < 0 || (size_t)rt <= cut);

        struct ppb_buf b = make_buf(p, cut);
        enum ppb_error e = PPB_OK;
        (void)decode_varint(&b, &e, 0); /* advances within bounds or errors */

        if (cut >= 1 && cut <= 10) /* valid varint ending exactly at EOF */
        {
            memset(p, 0xFF, cut);
            p[cut - 1] = 0x7F; /* clear the high bit: stop at the last byte */

            uint64_t exact = 0;
            int re = peek_varint(make_buf(p, cut), &exact);
            CHECK(re == (int)cut); /* reads exactly `cut` bytes, no over-read */
        }

        free(p);
    }

    {
        /* field 1 VARINT, 10-byte value: crosses the 8-byte fast/slow seam. */
        const uint8_t varint10[] = { 0x08, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01 };
        const struct ppb_encoded_tag tags[] = { PPB_TAG(1, PPB_WIRE_VARINT) };

        overread_truncation_sweep(varint10, sizeof(varint10), 1, tags);
    }

    {
        /* field 1 I64: buf_peek64 gated by buf_check(8). */
        const uint8_t i64[] = { 0x09, 1, 2, 3, 4, 5, 6, 7, 8 };
        const struct ppb_encoded_tag tags[] = { PPB_TAG(1, PPB_WIRE_I64) };

        overread_truncation_sweep(i64, sizeof(i64), 1, tags);
    }

    {
        /* field 1 I32: buf_peek32 gated by buf_check(4). */
        const uint8_t i32[] = { 0x0D, 0xAA, 0xBB, 0xCC, 0xDD };
        const struct ppb_encoded_tag tags[] = { PPB_TAG(1, PPB_WIRE_I32) };

        overread_truncation_sweep(i32, sizeof(i32), 1, tags);
    }

    {
        /* field 1 LEN, 2-byte length varint (len=128): length-then-payload buf_check. */
        uint8_t len128[3 + 128];

        len128[0] = 0x0A;
        len128[1] = 0x80;
        len128[2] = 0x01;
        memset(len128 + 3, 0x5A, 128);

        const struct ppb_encoded_tag tags[] = { PPB_TAG(1, PPB_WIRE_LEN) };

        overread_truncation_sweep(len128, sizeof(len128), 1, tags);
    }

    {
        /* field 16 I64: 2-byte tag, then buf_peek64. */
        const uint8_t tag16[] = { 0x81, 0x01, 1, 2, 3, 4, 5, 6, 7, 8 };
        const struct ppb_encoded_tag tags[] = { PPB_TAG(16, PPB_WIRE_I64) };

        overread_truncation_sweep(tag16, sizeof(tag16), 1, tags);
    }

    {
        /* 8-byte all-continuation tag: peek_tag fast path reads exactly 8, rejects. */
        const uint8_t overlong[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
        const struct ppb_encoded_tag tags[] = { PPB_TAG(1, PPB_WIRE_VARINT) };

        overread_truncation_sweep(overlong, sizeof(overlong), 1, tags);
    }
}

/*
 * prescan and lexn must agree byte-for-byte.  prescan validates the whole
 * message in one pass; lexn walks strictly-monotonic batches across repeated
 * calls.  Looping lexn to exhaustion must therefore accept iff prescan accepts
 * and consume exactly the bytes prescan blessed.  Each buffer is a heap copy at
 * exactly its length, ASan can easily detect over-reads.
 */

/*
 * Loop ppb_lexn (batch size `batch`) to exhaustion over an exact-sized heap
 * copy of `msg`.  Returns the total bytes consumed, or -1 if any batch reports
 * an error (matching prescan's negative-on-reject result).
 */
static ptrdiff_t
lexn_consume_all(const uint8_t *msg, size_t len, size_t num_fields, const struct ppb_encoded_tag *tags,
    size_t batch)
{
    uint8_t *p = malloc(len == 0 ? 1 : len);
    memcpy(p, msg, len);

    struct ppb_field fields[8];
    zero_fields(num_fields, fields);

    struct ppb_buf buf = make_buf(p, len);
    ptrdiff_t consumed = 0;

    for (;;)
    {
        const char *before = buf.buf;
        struct ppb_lexn_ret ret = ppb_lexn(&buf, num_fields, tags, fields, batch);

        consumed += (const char *)buf.buf - before;
        if (ret.status != PPB_OK)
        {
            consumed = -1;
            break;
        }

        /* No progress without an error means end-of-input. */
        if (buf.buf == before)
        {
            break;
        }
    }

    free(p);
    return consumed;
}

/*
 * For every truncation of `msg`, assert prescan and looped-lexn agree on
 * accept/reject and on bytes consumed, across several batch sizes.  A batch of
 * 1 forces one field per call; SIZE_MAX lets lexn's monotonic break decide the
 * batch boundaries, maximizing divergence from prescan.
 */
static void
prescan_lexn_consistency_sweep(const uint8_t *msg, size_t len, size_t num_fields,
    const struct ppb_encoded_tag *tags)
{
    static const size_t batches[] = { 1, 2, SIZE_MAX };

    CHECK(ppb_validate_tags(num_fields, tags) == PPB_OK);

    for (size_t cut = 0; cut <= len; cut++)
    {
        uint8_t *p = malloc(cut == 0 ? 1 : cut);
        memcpy(p, msg, cut);

        struct ppb_field fields[8];
        zero_fields(num_fields, fields);
        ptrdiff_t pscan = ppb_prescan(make_buf(p, cut), num_fields, tags, fields, SIZE_MAX);
        CHECK(pscan < 0 || (size_t)pscan <= cut);

        free(p);

        for (size_t bi = 0; bi < sizeof(batches) / sizeof(batches[0]); bi++)
        {
            ptrdiff_t lexn = lexn_consume_all(msg, cut, num_fields, tags, batches[bi]);

            CHECK(lexn < 0 || (size_t)lexn <= cut);
            CHECK((pscan >= 0) == (lexn >= 0));
            if (pscan >= 0 && lexn >= 0)
            {
                CHECK(lexn == pscan);
            }
        }
    }
}

static void
test_prescan_lexn_consistency_exact(void)
{
    printf("test_prescan_lexn_consistency_exact\n");

    /* All four wire types, fields in ascending order. */
    {
        struct ppb_encoded_tag tags[4];
        init_four_tags(tags);
        prescan_lexn_consistency_sweep(four_field_wire, sizeof(four_field_wire), 4, tags);
    }

    /*
     * Same four fields, wire order descending: lexn's monotonic break fires on
     * (nearly) every field, so a full-batch lexn call diverges most from
     * prescan and the loop takes several batches to catch up.
     */
    {
        static const uint8_t out_of_order[] = {
            0x25, 0x2a, 0x00, 0x00, 0x00,                         /* field 4 i32 42 */
            0x1a, 0x05, 0x68, 0x65, 0x6c, 0x6c, 0x6f,             /* field 3 LEN "hello" */
            0x11, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* field 2 i64 1 */
            0x08, 0x96, 0x01,                                     /* field 1 varint 150 */
        };
        struct ppb_encoded_tag tags[4];
        init_four_tags(tags);
        prescan_lexn_consistency_sweep(out_of_order, sizeof(out_of_order), 4, tags);
    }

    /* Repeated field: lexn yields one occurrence per batch. */
    {
        static const uint8_t repeated[] = {
            0x08, 0x01,       /* field 1 varint 1 */
            0x08, 0x96, 0x01, /* field 1 varint 150 */
            0x08, 0x01,       /* field 1 varint 1 */
        };
        struct ppb_encoded_tag tags[1] = { PPB_TAG(1, PPB_WIRE_VARINT) };
        prescan_lexn_consistency_sweep(repeated, sizeof(repeated), 1, tags);
    }

    /* Two sorted runs of the same fields: monotonic break between runs. */
    {
        static const uint8_t two_runs[] = {
            0x08, 0x01, 0x10, 0x02, /* field 1 = 1, field 2 = 2 */
            0x08, 0x03, 0x10, 0x04, /* field 1 = 3, field 2 = 4 */
        };
        struct ppb_encoded_tag tags[2] = {
            PPB_TAG(1, PPB_WIRE_VARINT),
            PPB_TAG(2, PPB_WIRE_VARINT),
        };
        prescan_lexn_consistency_sweep(two_runs, sizeof(two_runs), 2, tags);
    }

    /*
     * Unknown fields skipped without a catch-all: fields 2 and 4 fall to the
     * dummy slot in both walkers and must be consumed identically.
     */
    {
        struct ppb_encoded_tag tags[2] = {
            PPB_TAG(1, PPB_WIRE_VARINT),
            PPB_TAG(3, PPB_WIRE_LEN),
        };
        prescan_lexn_consistency_sweep(four_field_wire, sizeof(four_field_wire), 2, tags);
    }

    /* Catch-all schema: every field matches the wire-typed catch-all. */
    {
        struct ppb_encoded_tag tags[4] = {
            PPB_TAG(-1, PPB_WIRE_VARINT),
            PPB_TAG(-1, PPB_WIRE_I64),
            PPB_TAG(-1, PPB_WIRE_LEN),
            PPB_TAG(-1, PPB_WIRE_I32),
        };
        prescan_lexn_consistency_sweep(four_field_wire, sizeof(four_field_wire), 4, tags);
    }

    /* 10-byte varint value: crosses the 8-byte varint fast/slow seam. */
    {
        static const uint8_t varint10[] = {
            0x08,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0xFF,
            0x01,
        };
        struct ppb_encoded_tag tags[1] = { PPB_TAG(1, PPB_WIRE_VARINT) };
        prescan_lexn_consistency_sweep(varint10, sizeof(varint10), 1, tags);
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
    CHECK(fields[0].m.lost_distinct_u64 == 1); /* values 1, 150, 1 -> distinct */
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
    CHECK(fields[0].m.lost_distinct_u64 == 0); /* LEN never sets the flag */
    CHECK(fields[0].v.u64 == 0); /* because LEN always sets v.u64 = 0. */
}

/*
 * Exhaustive lost_distinct_u64 coverage: single-occurrence (never
 * flagged), equal-repeated atomic values (not flagged), distinct
 * atomic values (flagged), sticky-after-set, and LEN repeated with
 * distinct lengths (still not flagged: LEN is excluded by design).
 */
static void
test_prescan_lost_distinct_u64(void)
{
    printf("test_prescan_lost_distinct_u64\n");

    /* Single occurrence per wire type: lost_distinct_u64 stays 0. */
    {
        struct ppb_encoded_tag tags[4];
        init_four_tags(tags);
        struct ppb_field fields[4];
        zero_fields(4, fields);
        struct ppb_buf buf = make_buf(four_field_wire, sizeof(four_field_wire));
        ptrdiff_t ret = ppb_prescan(buf, 4, tags, fields, SIZE_MAX);
        CHECK(ret == (ptrdiff_t)sizeof(four_field_wire));

        for (size_t i = 0; i < 4; i++)
        {
            CHECK(fields[i].m.num_occurrences == 1);
            CHECK(fields[i].m.lost_distinct_u64 == 0);
        }
    }

    /* Two equal varint occurrences -> not flagged. */
    {
        static const uint8_t wire[] = {
            0x08, 0x07,  /* field 1 varint 7 */
            0x08, 0x07,  /* field 1 varint 7 */
        };
        struct ppb_encoded_tag tags[1] = { PPB_TAG(1, PPB_WIRE_VARINT) };
        struct ppb_field fields[1];
        zero_fields(1, fields);
        ptrdiff_t ret = ppb_prescan(make_buf(wire, sizeof(wire)), 1, tags, fields, SIZE_MAX);

        CHECK(ret == (ptrdiff_t)sizeof(wire));
        CHECK(fields[0].m.num_occurrences == 2);
        CHECK(fields[0].m.lost_distinct_u64 == 0);
    }

    /* Two distinct varint occurrences -> flagged. */
    {
        static const uint8_t wire[] = {
            0x08, 0x07,  /* field 1 varint 7 */
            0x08, 0x08,  /* field 1 varint 8 */
        };
        struct ppb_encoded_tag tags[1] = { PPB_TAG(1, PPB_WIRE_VARINT) };
        struct ppb_field fields[1];
        zero_fields(1, fields);
        ptrdiff_t ret = ppb_prescan(make_buf(wire, sizeof(wire)), 1, tags, fields, SIZE_MAX);

        CHECK(ret == (ptrdiff_t)sizeof(wire));
        CHECK(fields[0].m.num_occurrences == 2);
        CHECK(fields[0].m.lost_distinct_u64 == 1);
    }

    /* Equal-then-distinct varint: flag flips to 1 and stays set across a third equal occurrence. */
    {
        static const uint8_t wire[] = {
            0x08, 0x07,  /* 7 */
            0x08, 0x09,  /* 9 (different)  -> set flag */
            0x08, 0x09,  /* 9 (same again) -> still flagged */
        };
        struct ppb_encoded_tag tags[1] = { PPB_TAG(1, PPB_WIRE_VARINT) };
        struct ppb_field fields[1];
        zero_fields(1, fields);
        ptrdiff_t ret = ppb_prescan(make_buf(wire, sizeof(wire)), 1, tags, fields, SIZE_MAX);

        CHECK(ret == (ptrdiff_t)sizeof(wire));
        CHECK(fields[0].m.num_occurrences == 3);
        CHECK(fields[0].m.lost_distinct_u64 == 1);
    }

    /* I64: equal vs distinct bit patterns. */
    {
        static const uint8_t wire_eq[] = {
            0x11, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* 1i64 */
            0x11, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* 1i64 */
        };
        static const uint8_t wire_ne[] = {
            0x11, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* 1i64 */
            0x11, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* 2i64 */
        };
        struct ppb_encoded_tag tags[1] = { PPB_TAG(2, PPB_WIRE_I64) };
        struct ppb_field fields[1];

        zero_fields(1, fields);
        CHECK(ppb_prescan(make_buf(wire_eq, sizeof(wire_eq)), 1, tags, fields, SIZE_MAX) ==
            (ptrdiff_t)sizeof(wire_eq));
        CHECK(fields[0].m.lost_distinct_u64 == 0);

        zero_fields(1, fields);
        CHECK(ppb_prescan(make_buf(wire_ne, sizeof(wire_ne)), 1, tags, fields, SIZE_MAX) ==
            (ptrdiff_t)sizeof(wire_ne));
        CHECK(fields[0].m.lost_distinct_u64 == 1);
    }

    /* I32: equal vs distinct bit patterns. */
    {
        static const uint8_t wire_eq[] = {
            0x25, 0x2a, 0x00, 0x00, 0x00,  /* 42i32 */
            0x25, 0x2a, 0x00, 0x00, 0x00,  /* 42i32 */
        };
        static const uint8_t wire_ne[] = {
            0x25, 0x2a, 0x00, 0x00, 0x00,  /* 42i32 */
            0x25, 0x2b, 0x00, 0x00, 0x00,  /* 43i32 */
        };
        struct ppb_encoded_tag tags[1] = { PPB_TAG(4, PPB_WIRE_I32) };
        struct ppb_field fields[1];

        zero_fields(1, fields);
        CHECK(ppb_prescan(make_buf(wire_eq, sizeof(wire_eq)), 1, tags, fields, SIZE_MAX) ==
            (ptrdiff_t)sizeof(wire_eq));
        CHECK(fields[0].m.lost_distinct_u64 == 0);

        zero_fields(1, fields);
        CHECK(ppb_prescan(make_buf(wire_ne, sizeof(wire_ne)), 1, tags, fields, SIZE_MAX) ==
            (ptrdiff_t)sizeof(wire_ne));
        CHECK(fields[0].m.lost_distinct_u64 == 1);
    }

    /* LEN: the flag is always 0 (u64 is always zero). */
    {
        static const uint8_t wire[] = {
            0x0a, 0x00,                                /* len 0 */
            0x0a, 0x05, 0x68, 0x65, 0x6c, 0x6c, 0x6f,  /* len 5 "hello" */
        };
        struct ppb_encoded_tag tags[1] = { PPB_TAG(1, PPB_WIRE_LEN) };
        struct ppb_field fields[1];
        zero_fields(1, fields);
        ptrdiff_t ret = ppb_prescan(make_buf(wire, sizeof(wire)), 1, tags, fields, SIZE_MAX);

        CHECK(ret == (ptrdiff_t)sizeof(wire));
        CHECK(fields[0].m.num_occurrences == 2);
        CHECK(fields[0].m.lost_distinct_u64 == 0);
        CHECK(fields[0].v.u64 == 0);
    }
}

static void
test_validate_tags(void)
{
    printf("test_validate_tags\n");

    /* Empty array: vacuously valid. */
    CHECK(ppb_validate_tags(0, NULL) == PPB_OK);

    /* Valid single-field array. */
    {
        struct ppb_encoded_tag tags[1] = { PPB_TAG(1, PPB_WIRE_VARINT) };
        CHECK(ppb_validate_tags(1, tags) == PPB_OK);
    }

    /* Unsorted fields. */
    {
        struct ppb_encoded_tag tags[2] = { PPB_TAG(2, PPB_WIRE_VARINT), PPB_TAG(1, PPB_WIRE_VARINT) };
        CHECK(ppb_validate_tags(2, tags) == PPB_ERROR_UNSORTED_FIELD_ARR);
    }

    /* Unsorted field after sorted run. */
    {
        struct ppb_encoded_tag tags[4] = {
            PPB_TAG(1, PPB_WIRE_VARINT),
            PPB_TAG(2, PPB_WIRE_VARINT),
            PPB_TAG(4, PPB_WIRE_VARINT),
            PPB_TAG(3, PPB_WIRE_VARINT),
        };
        CHECK(ppb_validate_tags(4, tags) == PPB_ERROR_UNSORTED_FIELD_ARR);
    }

    /* Repeated tag. */
    {
        struct ppb_encoded_tag tags[2] = { PPB_TAG(1, PPB_WIRE_VARINT), PPB_TAG(1, PPB_WIRE_VARINT) };
        CHECK(ppb_validate_tags(2, tags) == PPB_ERROR_UNSORTED_FIELD_ARR);
    }

    /* Sentinel: all bits values 0-7 (field number 0 is forbidden; bits < 8 triggers the check). */
    for (uint64_t b = 0; b <= 7; b++)
    {
        struct ppb_encoded_tag tags[1] = { { .bits = b } };
        CHECK(ppb_validate_tags(1, tags) == PPB_ERROR_SENTINEL_FIELD_ARR);
    }
}

/* Sentinel check fires before the sortedness check. */
static void
test_validate_tags_sentinel_first(void)
{
    printf("test_validate_tags_sentinel_first\n");

    /*
     * bits=0 is a sentinel, and the second entry would also be unsorted.
     * Sentinel error must be returned, not UNSORTED.
     */
    struct ppb_encoded_tag tags[2] = { { .bits = 0 }, { .bits = 0 } };
    CHECK(ppb_validate_tags(2, tags) == PPB_ERROR_SENTINEL_FIELD_ARR);

    /* Valid first entry, invalid (unsorted) second: UNSORTED returned. */
    {
        struct ppb_encoded_tag tags2[2] = { PPB_TAG(2, PPB_WIRE_VARINT), PPB_TAG(1, PPB_WIRE_VARINT) };
        CHECK(ppb_validate_tags(2, tags2) == PPB_ERROR_UNSORTED_FIELD_ARR);
    }
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

/* multiple instances of the same field, we expect one per lexn call. */
static void
test_lexn_repeated_one_per_call(void)
{
    printf("test_lexn_repeated_one_per_call\n");

    static const uint8_t wire[] = {
        0x08, 0x01,        /* field 1 varint 1  (1-byte value) */
        0x08, 0x96, 0x01,  /* field 1 varint 150 (2-byte value) */
        0x08, 0x01,        /* field 1 varint 1  (1-byte value) */
    };

    struct ppb_encoded_tag tags[1] = { PPB_TAG(1, PPB_WIRE_VARINT) };
    struct ppb_field fields[1];
    zero_fields(1, fields);

    struct ppb_buf buf = make_buf(wire, sizeof(wire));
    struct ppb_lexn_ret ret;

    ret = ppb_lexn(&buf, 1, tags, fields, SIZE_MAX);
    CHECK(ret.status == PPB_OK);
    CHECK(ret.first_field == 0);
    CHECK(ret.field_range == 1);
    CHECK(fields[0].v.u64 == 1);

    ret = ppb_lexn(&buf, 1, tags, fields, SIZE_MAX);
    CHECK(ret.status == PPB_OK);
    CHECK(ret.first_field == 0);
    CHECK(ret.field_range == 1);
    CHECK(fields[0].v.u64 == 150);

    ret = ppb_lexn(&buf, 1, tags, fields, SIZE_MAX);
    CHECK(ret.status == PPB_OK);
    CHECK(ret.first_field == 0);
    CHECK(ret.field_range == 1);
    CHECK(fields[0].v.u64 == 1);

    CHECK(buf.size == 0);
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

/*
 * Test edge cases around num_occurrences and LEN fields that are
 * practically only a concern on 32-bit platforms.
 */
static void
test_32b_size_edge_cases()
{
    printf("test_32b_size_edge_cases\n");

    /*
     * We must have enough room for num_occurrences, and each
     * occurrence needs at least 2 bytes (one for the tag, another for
     * the value).
     */
    {
        struct ppb_field_meta m = { 0 };
        size_t max_occurrences = (size_t)PTRDIFF_MAX / 2;

        /*
         * GCC warns about the very issue we're trying to confirm
         * is provably not a concern for us.
         */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
        m.num_occurrences = max_occurrences;
#pragma GCC diagnostic pop

        m.lost_distinct_u64 = 1;

        CHECK(m.num_occurrences == max_occurrences);
        CHECK(m.lost_distinct_u64 == 1);
    }

    /*
     * On 32-bit platforms, size_t is narrower than uint64_t, and we
     * must make sure to bound check against the uint64_t LENgth field
     * before truncating to size_t.
     *
     * Test that with a couple different ways to stick a LEN field in
     * front of an 8-byte payload.
     */
    {
        static const struct
        {
            uint8_t varint[10];
            size_t varint_len;
            size_t trailing;
            bool accept;
        } cases[] = {
            { { 0x08 }, 1, 8, true },                           /* len 8: exact fit */
            { { 0x80, 0x80, 0x80, 0x80, 0x10 }, 5, 8, false },  /* 2^32: low 32 bits == 0 */
            { { 0x88, 0x80, 0x80, 0x80, 0x10 }, 5, 8, false },  /* 2^32 + 8: low 32 bits == 8 */
            { { 0xff, 0xff, 0xff, 0xff, 0xff,                   /* UINT64_MAX */
                  0xff, 0xff, 0xff, 0xff, 0x01 },
                10, 0, false },
        };

        const struct ppb_encoded_tag tags[1] = { PPB_TAG(1, PPB_WIRE_LEN) };

        for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++)
        {
            /* first byte is the tag, remainder is the length, then the payload. */
            uint8_t wire[1 + 10 + 8] = { 0x0a };
            size_t wire_size = 1 + cases[c].varint_len + cases[c].trailing;
            memcpy(wire + 1, cases[c].varint, cases[c].varint_len);

            struct ppb_field fields[1] = { 0 };

            struct ppb_buf buf = make_buf(wire, wire_size);
            ptrdiff_t ret = ppb_prescan(buf, 1, tags, fields, SIZE_MAX);

            if (cases[c].accept)
            {
                CHECK(ret == (ptrdiff_t)wire_size);
                CHECK(fields[0].m.num_occurrences == 1);
                CHECK(fields[0].v.payload.size == cases[c].trailing);
                CHECK(fields[0].v.payload.buf == wire + 1 + cases[c].varint_len);
            }
            else
            {
                CHECK(ret == PPB_ERROR_TRUNCATED_DATA);
            }
        }
    }
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

            {
                CHECK(squish_varint(encoded) == expected);
                CHECK(squish_varint_portable(encoded) == expected);
            }

            uint8_t tag_bytes[8];
            for (size_t bi = 0; bi < sizeof(tag_bytes); bi++)
                tag_bytes[bi] = (uint8_t)(encoded >> (8 * bi));

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

/* Both squish_varint implementations must agree on arbitrary uint64_t inputs. */
static void
test_squish_varint_equivalence(void)
{
    printf("test_squish_varint_equivalence\n");

    static const uint64_t cases[] = {
        0,
        UINT64_MAX,
        0xAAAAAAAAAAAAAAAAULL,  /* alternating 10/01 per bit */
        0x5555555555555555ULL,  /* alternating 01/10 per bit */
        0x7F7F7F7F7F7F7F7FULL,  /* all data bits, no continuation bits */
        0x8080808080808080ULL,  /* continuation bits only */
        0xFF00FF00FF00FF00ULL,
        0x00FF00FF00FF00FFULL,
        0x0101010101010101ULL,
        0x0000000000000001ULL,
        0x0000000000000080ULL,
        0x0100000000000000ULL,
        0x8000000000000000ULL,
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        CHECK(squish_varint(cases[i]) == squish_varint_portable(cases[i]));

    /*
     * Both squish_varint paths are GF(2)-linear in x: the BMI2 _pext_u64 is a
     * bit-gather, and the portable parallel-merge uses only (x & const), |, and
     * (x >> const) where every | combines bit-disjoint operands, so it acts as
     * XOR.  A GF(2)-linear map on a uint64_t is fully determined by its action
     * on the 64 basis vectors 1<<i, so agreement on those proves agreement on
     * all 2^64 inputs.  WP can only model the portable path and each binary
     * compiles just one of the two, so this is the sole check that the BMI2 hot
     * path matches the proven one; it bites only on an x86-64 -mbmi2 build (and
     * under qemu-x86_64), reducing to a tautology elsewhere.
     */
    for (int bit = 0; bit < 64; bit++)
    {
        uint64_t x = (uint64_t)1 << bit;

        CHECK(squish_varint(x) == squish_varint_portable(x));
    }
}

/*
 * Encode a single fixed32 (wire type 5) field into `out`.
 * Returns the number of bytes written.
 */
static size_t
encode_i32_field(uint64_t field_num, uint32_t value, uint8_t *out)
{
    uint64_t tag = field_num << 3 | PPB_WIRE_I32;
    size_t n = 0;
    while (tag >= 0x80)
    {
        out[n++] = (uint8_t)(tag | 0x80);
        tag >>= 7;
    }
    out[n++] = (uint8_t)tag;
    for (size_t i = 0; i < 4; i++)
        out[n + i] = (uint8_t)(value >> (8 * i));
    return n + 4;
}

/*
 * fields[].v is zeroed before handle_field so that v.ptr stays NULL
 * when parsing fails.  Without that clear a second occurrence of
 * field 1 that fails would leave the ptr from the first (successful)
 * occurrence.
 *
 * Wire: valid field 1 varint, then field 1 with a truncated value.
 * After prescan returns an error, fields[0].v.ptr must be NULL.
 */
static void
test_prescan_v_ptr_null_on_error(void)
{
    printf("test_prescan_v_ptr_null_on_error\n");

    static const uint8_t wire[] = {
        0x08, 0x01,  /* field 1 varint 1 (valid) */
        0x08, 0x80,  /* field 1 varint — truncated, no terminating byte */
    };

    struct ppb_encoded_tag tags[1] = { PPB_TAG(1, PPB_WIRE_VARINT) };
    struct ppb_field fields[1];
    zero_fields(1, fields);

    struct ppb_buf buf = make_buf(wire, sizeof(wire));
    ptrdiff_t ret = ppb_prescan(buf, 1, tags, fields, SIZE_MAX);

    CHECK(ret == PPB_ERROR_TRUNCATED_DATA);
    CHECK(fields[0].v.ptr == NULL);
}

/*
 * Same guarantee for lexn.  First call succeeds and sets v.ptr;
 * second call encounters a truncated value for the same field and
 * must leave v.ptr == NULL on return.
 */
static void
test_lexn_v_ptr_null_on_error(void)
{
    printf("test_lexn_v_ptr_null_on_error\n");

    static const uint8_t first_wire[] = {
        0x08, 0x01,  /* field 1 varint 1 (valid) */
    };
    static const uint8_t second_wire[] = {
        0x08, 0x80,  /* field 1 varint — truncated */
    };

    struct ppb_encoded_tag tags[1] = { PPB_TAG(1, PPB_WIRE_VARINT) };
    struct ppb_field fields[1];
    zero_fields(1, fields);

    /* First call: succeeds, v.ptr gets set to the tag byte. */
    struct ppb_buf buf = make_buf(first_wire, sizeof(first_wire));
    struct ppb_lexn_ret ret = ppb_lexn(&buf, 1, tags, fields, SIZE_MAX);
    CHECK(ret.status == PPB_OK);
    CHECK(ret.field_range == 1);
    CHECK(fields[0].v.ptr == first_wire);  /* ptr was set */

    /* Second call: truncated value — handle_field fails, ptr must be NULL. */
    buf = make_buf(second_wire, sizeof(second_wire));
    ret = ppb_lexn(&buf, 1, tags, fields, SIZE_MAX);
    CHECK(ret.status == PPB_ERROR_TRUNCATED_DATA);
    CHECK(fields[0].v.ptr == NULL);
}

/*
 * Systematic binary-search exercise: 8 even-numbered I32 tags (2,4,...,16).
 * For each prefix length 0..8, probe with field numbers 1..17 and verify
 * that odd tags are skipped and even tags in range are found at the
 * correct index.
 */
static void
test_prescan_bsearch_systematic(void)
{
    printf("test_prescan_bsearch_systematic\n");

    struct ppb_encoded_tag all_tags[8] = {
        PPB_TAG(2, PPB_WIRE_I32),
        PPB_TAG(4, PPB_WIRE_I32),
        PPB_TAG(6, PPB_WIRE_I32),
        PPB_TAG(8, PPB_WIRE_I32),
        PPB_TAG(10, PPB_WIRE_I32),
        PPB_TAG(12, PPB_WIRE_I32),
        PPB_TAG(14, PPB_WIRE_I32),
        PPB_TAG(16, PPB_WIRE_I32),
    };

    for (size_t prefix_len = 0; prefix_len <= 8; prefix_len++)
    {
        CHECK(ppb_validate_tags(prefix_len, all_tags) == PPB_OK);

        for (uint64_t probe = 1; probe <= 17; probe++)
        {
            uint8_t wire[16];
            size_t wire_len = encode_i32_field(probe, (uint32_t)probe, wire);

            /*
             * Match iff probe is even, in [2..16], and its table
             * index (probe/2 - 1) falls within the prefix.
             */
            bool should_match = (probe % 2 == 0) && (probe >= 2) && (probe / 2 <= prefix_len);
            size_t expected_idx = (size_t)(probe / 2 - 1);

            struct ppb_field fields[8];
            zero_fields(8, fields);
            struct ppb_buf buf = make_buf(wire, wire_len);

            ptrdiff_t r = ppb_prescan(buf, prefix_len, all_tags, fields, SIZE_MAX);
            CHECK(r == (ptrdiff_t)wire_len);

            for (size_t i = 0; i < prefix_len; i++)
            {
                if (should_match && i == expected_idx)
                {
                    CHECK(fields[i].m.num_occurrences == 1);
                    CHECK(fields[i].m.total_bytes == 4);
                }
                else
                {
                    CHECK(fields[i].m.num_occurrences == 0);
                }
            }
        }
    }
}

/*
 * Check ppb_prescan metadata against per-field stats accumulated by
 * lexing one field at a time.  We have similar code in the fuzzing
 * harness and in picoscope, but this is a crucial invariant.
 */
static void
test_cross_check_meta(void)
{
    printf("test_cross_check_meta\n");

    struct ppb_encoded_tag tags[4];
    init_four_tags(tags);
    CHECK(ppb_validate_tags(4, tags) == PPB_OK);

    struct ppb_buf buf = make_buf(four_field_wire, sizeof(four_field_wire));
    struct ppb_field fields[4];
    zero_fields(4, fields);

    ptrdiff_t scan = ppb_prescan(buf, 4, tags, fields, SIZE_MAX);
    CHECK(scan == (ptrdiff_t)sizeof(four_field_wire));

    struct ppb_field_meta prescan_m[4];
    for (size_t i = 0; i < 4; i++)
        prescan_m[i] = fields[i].m;

    /* Lex one field at a time and accumulate stats into lexn_m. */
    struct ppb_field_meta lexn_m[4];
    memset(lexn_m, 0, sizeof(lexn_m));

    /*
     * Per-field record of the previous occurrence's u64, mirroring
     * what handle_field uses to compute lost_distinct_u64.
     */
    uint64_t prev_u64[4] = { 0 };

    buf = make_buf(four_field_wire, sizeof(four_field_wire));
    zero_fields(4, fields);

    while (buf.size > 0)
    {
        struct ppb_lexn_ret ret = ppb_lexn(&buf, 4, tags, fields, 1);
        CHECK(ret.status == PPB_OK);
        if (ret.field_range > 0)
        {
            size_t i = ret.first_field;
            unsigned wt = (unsigned)(tags[i].bits & 7);
            bool first = lexn_m[i].num_occurrences == 0;
            if (wt == PPB_WIRE_LEN)
            {
                size_t sz = fields[i].v.payload.size;
                lexn_m[i].total_bytes += sz;
                if (sz > 0 && (lexn_m[i].min_nonzero_bytes == 0 || sz < lexn_m[i].min_nonzero_bytes))
                    lexn_m[i].min_nonzero_bytes = sz;
                if (sz > lexn_m[i].max_bytes)
                    lexn_m[i].max_bytes = sz;
            }
            else
            {
                uint64_t v = fields[i].v.u64;

                if (!first && v != prev_u64[i])
                    lexn_m[i].lost_distinct_u64 = 1;

                prev_u64[i] = v;
            }

            lexn_m[i].num_occurrences++;
        }
    }

    for (size_t i = 0; i < 4; i++)
    {
        CHECK(prescan_m[i].num_occurrences == lexn_m[i].num_occurrences);
        CHECK(prescan_m[i].lost_distinct_u64 == lexn_m[i].lost_distinct_u64);

        unsigned wt = (unsigned)(tags[i].bits & 7);

        if (wt == PPB_WIRE_LEN)
        {
            CHECK(prescan_m[i].total_bytes == lexn_m[i].total_bytes);
            CHECK(prescan_m[i].min_nonzero_bytes == lexn_m[i].min_nonzero_bytes);
            CHECK(prescan_m[i].max_bytes == lexn_m[i].max_bytes);
        }

        if (wt == PPB_WIRE_I32)
            CHECK(prescan_m[i].total_bytes == 4 * prescan_m[i].num_occurrences);

        if (wt == PPB_WIRE_I64)
            CHECK(prescan_m[i].total_bytes == 8 * prescan_m[i].num_occurrences);
    }
}

/*
 * Two ppb_prescan calls on the same buffer with zero-initialized
 * fields arrays must produce byte-identical metadata.  We're just
 * testing determinism, and it's trivially true given statelessness,
 * but it's nice to be explicit.
 */
static void
test_prescan_deterministic(void)
{
    printf("test_prescan_deterministic\n");

    struct ppb_encoded_tag tags[4];
    init_four_tags(tags);

    struct ppb_buf buf = make_buf(four_field_wire, sizeof(four_field_wire));

    struct ppb_field fields1[4], fields2[4];
    zero_fields(4, fields1);
    zero_fields(4, fields2);

    ptrdiff_t r1 = ppb_prescan(buf, 4, tags, fields1, SIZE_MAX);
    ptrdiff_t r2 = ppb_prescan(buf, 4, tags, fields2, SIZE_MAX);

    CHECK(r1 == r2);
    for (size_t i = 0; i < 4; i++)
        CHECK(memcmp(&fields1[i].m, &fields2[i].m, sizeof(fields1[i].m)) == 0);
}

/*
 * Verify that the typed union members .u32, .i32, .f, and .d alias
 * v.u64 correctly regardless of host endianness.  On big-endian the
 * overlay struct places 32-bit members at offset 4 so they always
 * alias the low half of u64.
 */
static void
test_field_value_union_overlay(void)
{
    printf("test_field_value_union_overlay\n");

    struct ppb_encoded_tag tags[2] = {
        PPB_TAG(1, PPB_WIRE_I32),
        PPB_TAG(2, PPB_WIRE_I64),
    };
    CHECK(ppb_validate_tags(2, tags) == PPB_OK);

    struct ppb_field fields[2];
    struct ppb_lexn_ret ret;

    /*
     * Non-trivial values: 0xDEADBEEF exercises all 4 bytes of the
     * low-32-bit half, catching offset-zero vs offset-4 placement.
     * max_lexed_fields=1 peels off one field per call.
     */
    {
        static const uint8_t wire[] = {
            0x0d, 0xef, 0xbe, 0xad, 0xde,                               /* f1: 0xDEADBEEF */
            0x11, 0xef, 0xbe, 0xad, 0xde, 0xbe, 0xba, 0xfe, 0xca,       /* f2: 0xCAFEBABEDEADBEEF */
        };
        struct ppb_buf buf = make_buf(wire, sizeof(wire));

        zero_fields(2, fields);

        ret = ppb_lexn(&buf, 2, tags, fields, /*max_lexed_fields=*/1);
        CHECK(ret.status == PPB_OK);
        CHECK(ret.first_field == 0);
        CHECK(ret.field_range == 1);
        CHECK(fields[0].v.u64 == 0xDEADBEEFULL);
        CHECK(fields[0].v.u32 == 0xDEADBEEFUL);
        CHECK(fields[0].v.i32 == (int32_t)0xDEADBEEFUL);

        ret = ppb_lexn(&buf, 2, tags, fields, /*max_lexed_fields=*/1);
        CHECK(ret.status == PPB_OK);
        CHECK(ret.first_field == 1);
        CHECK(ret.field_range == 1);
        CHECK(fields[1].v.u64 == 0xCAFEBABEDEADBEEFULL);
        CHECK(fields[1].v.i64 == (int64_t)0xCAFEBABEDEADBEEFULL);
    }

    /*
     * float: 1.0f = IEEE 754 0x3F800000, stored LE on wire.
     * Verifies .f correctly aliases the low 32 bits on both LE and BE.
     */
    {
        static const uint8_t wire[] = {
            0x0d, 0x00, 0x00, 0x80, 0x3f, /* f1: 1.0f */
        };
        struct ppb_buf buf = make_buf(wire, sizeof(wire));

        zero_fields(2, fields);

        ret = ppb_lexn(&buf, 2, tags, fields, 4);
        CHECK(ret.status == PPB_OK);
        CHECK(ret.first_field == 0);
        CHECK(ret.field_range == 1);
        CHECK(fields[0].v.f == 1.0f);
    }

    /*
     * float: -0.0f = IEEE 754 0x80000000.
     * The sign bit exercises the top bit of the low 32-bit half.
     */
    {
        static const uint8_t wire[] = {
            0x0d, 0x00, 0x00, 0x00, 0x80, /* f1: -0.0f */
        };
        struct ppb_buf buf = make_buf(wire, sizeof(wire));

        zero_fields(2, fields);

        ret = ppb_lexn(&buf, 2, tags, fields, 4);
        CHECK(ret.status == PPB_OK);
        CHECK(ret.first_field == 0);
        CHECK(ret.field_range == 1);
        CHECK(fields[0].v.u32 == 0x80000000UL);
        CHECK(fields[0].v.i32 == (int32_t)0x80000000UL);
        CHECK(fields[0].v.f == -0.0f);
        /* -0.0f compares equal to 0.0f; also check bit pattern via u32. */
        CHECK(fields[0].v.u32 == 0x80000000UL);
    }

    /*
     * double: IEEE 754 bits, stored LE on wire.
     * 1.0 = 0x3FF0000000000000.
     * Verifies .d aliases v.u64 correctly (always at offset 0).
     */
    {
        static const uint8_t wire[] = {
            0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf0, 0x3f, /* f2: 1.0 */
        };
        struct ppb_buf buf = make_buf(wire, sizeof(wire));

        zero_fields(2, fields);

        ret = ppb_lexn(&buf, 2, tags, fields, 4);
        CHECK(ret.status == PPB_OK);
        CHECK(ret.first_field == 1);
        CHECK(ret.field_range == 1);
        CHECK(fields[1].v.d == 1.0);
        CHECK(fields[1].v.i64 == (int64_t)0x3FF0000000000000ULL);
    }
}

int
main(void)
{
    test_zag();
    test_zag32();
    test_saturating_subzu();
    test_saturating_range_u32();
    test_peekn();
    test_decode_varint();
    test_decode_varint_10byte();
    test_peek_varint_error();
    test_peek_tag_error();
    test_peek_tag_range();
    test_overread_boundary_exact();
    test_prescan_lexn_consistency_exact();

    test_prescan_known_fields();
    test_prescan_repeated_fields();
    test_prescan_repeated_len();
    test_prescan_lost_distinct_u64();
    test_validate_tags();
    test_validate_tags_sentinel_first();
    test_prescan_unknown_fields();
    test_prescan_max_fields();
    test_prescan_zero_tag();
    test_prescan_corrupt_tag();
    test_prescan_truncated_tag();

    test_lexn_known_fields_in_order();
    test_lexn_one_per_call();
    test_lexn_repeated_one_per_call();
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

    test_32b_size_edge_cases();

    test_tag_macros();
    test_tag_varint_roundtrip();
    test_squish_varint_equivalence();

    test_prescan_v_ptr_null_on_error();
    test_lexn_v_ptr_null_on_error();

    test_prescan_bsearch_systematic();

    test_cross_check_meta();
    test_prescan_deterministic();

    test_field_value_union_overlay();

    printf("\n%d checks, %d failures\n", g_check_count, g_fail_count);
    return g_fail_count > 0 ? 1 : 0;
}
