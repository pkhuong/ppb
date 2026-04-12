/*
 * LibFuzzer harness for the PPB public interface.
 *
 * Exercises public functions (ppb_zag, ppb_decode_varint,
 * ppb_prescan*, ppb_lexn*) against arbitrary byte sequences and
 * double-checks ACSL postconditions at runtime.
 */
#include "ppb/ppb.h"

#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define POSTCOND(cond)        \
    do                        \
    {                         \
        if (!(cond))          \
            __builtin_trap(); \
    } while (0)

/* Four specific fields: matches the four_field_wire[] test fixture. */
static const struct ppb_encoded_tag SPECIFIC_TAGS[] = {
    PPB_TAG(1, PPB_WIRE_VARINT), /* bits = 0x08 */
    PPB_TAG(2, PPB_WIRE_I64),    /* bits = 0x11 */
    PPB_TAG(3, PPB_WIRE_LEN),    /* bits = 0x1A */
    PPB_TAG(4, PPB_WIRE_I32),    /* bits = 0x25 */
};
#define N_SPECIFIC 4

/* Catch-all: one entry per wire type, using field -1 (= UINT64_MAX field). */
static const struct ppb_encoded_tag CATCHALL_TAGS[] = {
    PPB_TAG(-1, PPB_WIRE_VARINT), /* bits = UINT64_MAX<<3 | 0 */
    PPB_TAG(-1, PPB_WIRE_I64),    /* bits = UINT64_MAX<<3 | 1 */
    PPB_TAG(-1, PPB_WIRE_LEN),    /* bits = UINT64_MAX<<3 | 2 */
    PPB_TAG(-1, PPB_WIRE_I32),    /* bits = UINT64_MAX<<3 | 5 */
};
#define N_CATCHALL 4

/* Mixed: two specific fields followed by four catch-alls. */
static const struct ppb_encoded_tag MIXED_TAGS[] = {
    PPB_TAG(1, PPB_WIRE_VARINT),  /* bits = 0x08 */
    PPB_TAG(3, PPB_WIRE_LEN),     /* bits = 0x1A */
    PPB_TAG(-1, PPB_WIRE_VARINT), /* bits = UINT64_MAX<<3 | 0 */
    PPB_TAG(-1, PPB_WIRE_I64),    /* bits = UINT64_MAX<<3 | 1 */
    PPB_TAG(-1, PPB_WIRE_LEN),    /* bits = UINT64_MAX<<3 | 2 */
    PPB_TAG(-1, PPB_WIRE_I32),    /* bits = UINT64_MAX<<3 | 5 */
};
#define N_MIXED 6

/* Maximum field arrays (sized to the largest configuration). */
#define MAX_FIELDS N_MIXED

/*
 * Verify that every tag array is strictly sorted ascending and that every
 * element has bits > 7.  This runs once before any fuzzer input is processed.
 */
__attribute__((constructor)) static void
check_tag_ordering(void)
{
    for (size_t i = 1; i < N_SPECIFIC; i++)
        assert(SPECIFIC_TAGS[i - 1].bits < SPECIFIC_TAGS[i].bits);
    for (size_t i = 0; i < N_SPECIFIC; i++)
        assert(SPECIFIC_TAGS[i].bits > 7);

    for (size_t i = 1; i < N_CATCHALL; i++)
        assert(CATCHALL_TAGS[i - 1].bits < CATCHALL_TAGS[i].bits);
    for (size_t i = 0; i < N_CATCHALL; i++)
        assert(CATCHALL_TAGS[i].bits > 7);

    for (size_t i = 1; i < N_MIXED; i++)
        assert(MIXED_TAGS[i - 1].bits < MIXED_TAGS[i].bits);
    for (size_t i = 0; i < N_MIXED; i++)
        assert(MIXED_TAGS[i].bits > 7);
}

static inline void
check_buf_valid(struct ppb_buf buf, const uint8_t *data, size_t size)
{
    POSTCOND(buf.size <= size);
    POSTCOND((const char *)buf.buf + buf.size == (const char *)data + size);
}

static void
fuzz_zag(const uint8_t *data, size_t size)
{
    uint64_t x = 0;
    size_t n = size < 8 ? size : 8;
    memcpy(&x, data, n);

    for (size_t rep = 0; rep < 2; rep++)
    {
        int64_t r = ppb_zag(x);

        /* Admitted postcondition: sint32 behavior of ppb_zag. */
        if (x < (1UL << 32))
        {
            POSTCOND(r >= (int64_t)INT32_MIN);
            POSTCOND(r <= (int64_t)INT32_MAX);
        }

        x &= UINT32_MAX;
    }
}

static void
fuzz_decode_varint(const uint8_t *data, size_t size)
{
    struct ppb_buf buf = { data, size };
    enum ppb_error error = PPB_OK;

    do
    {
        uint64_t val = ppb_decode_varint(&buf, &error);

        POSTCOND((int)error <= 0);
        if (error != PPB_OK)
            POSTCOND(val == 0);

        check_buf_valid(buf, data, size);
    } while (error == PPB_OK && buf.size > 0);
}

static void
do_prescan(const uint8_t *data, size_t size, size_t num_fields, const struct ppb_encoded_tag *tags,
    struct ppb_field *fields)
{
    struct ppb_buf buf = { data, size };

    {
        memset(fields, 0, num_fields * sizeof(fields[0]));
        ptrdiff_t r = ppb_prescan(buf, num_fields, tags, fields, SIZE_MAX);

        if (r >= 0)
        {
            POSTCOND((size_t)r <= size);
            POSTCOND(num_fields == 0 || tags[0].bits > 7);
        }
    }

    {
        memset(fields, 0, num_fields * sizeof(fields[0]));
        ptrdiff_t r = ppb_prescan(buf, num_fields, tags, fields, 0);

        if (r >= 0)
        {
            POSTCOND((size_t)r <= size);
        }
    }

    size_t limits[] = { 0, size / 2, size, SIZE_MAX };
    for (size_t li = 0; li < sizeof(limits) / sizeof(limits[0]); li++)
    {
        size_t limit = limits[li];
        memset(fields, 0, num_fields * sizeof(fields[0]));
        ptrdiff_t r = ppb_prescan_with_hard_limit(buf, limit, num_fields, tags, fields, SIZE_MAX);

        if (r >= 0)
        {
            POSTCOND((size_t)r <= size);
            POSTCOND((size_t)r <= limit);
            POSTCOND(num_fields == 0 || tags[0].bits > 7);
        }
    }

    for (size_t li = 0; li < sizeof(limits) / sizeof(limits[0]); li++)
    {
        size_t limit = limits[li];
        memset(fields, 0, num_fields * sizeof(fields[0]));

        ptrdiff_t r = ppb_prescan_with_soft_limit(buf, limit, num_fields, tags, fields, SIZE_MAX);
        if (r >= 0)
        {
            POSTCOND((size_t)r <= size);
            POSTCOND(num_fields == 0 || tags[0].bits > 7);
        }
    }
}

static inline void
check_lexn_postconds(struct ppb_lexn_ret ret, struct ppb_buf buf, size_t num_fields,
    const struct ppb_field_meta *meta_snapshot, const struct ppb_field *fields, size_t old_size,
    size_t max_lexed_fields, size_t limit, const uint8_t *data, size_t size)
{
    /* buf_valid postcondition (partly admitted in Frama-C). */
    check_buf_valid(buf, data, size);

    /* Admitted postcondition: fields[].m unchanged across ppb_lexn. */
    for (size_t i = 0; i < num_fields; i++)
        POSTCOND(memcmp(&fields[i].m, &meta_snapshot[i], sizeof(meta_snapshot[0])) == 0);

    /* ppb_lexn_ret range invariant (header-doc guarantee). */
    POSTCOND(ret.field_range == UINT32_MAX || ret.first_field + ret.field_range <= num_fields);

    /* Progress postcondition. */
    if (old_size > 0 && limit > 0 && max_lexed_fields > 0)
        POSTCOND(ret.status != PPB_OK || buf.size < old_size);
}

static struct ppb_lexn_ret
do_lexn_call(struct ppb_buf *buf, size_t num_fields, const struct ppb_encoded_tag *tags,
    struct ppb_field *fields, size_t max_lexed_fields, const uint8_t *data, size_t size)
{
    struct ppb_field_meta meta_snapshot[MAX_FIELDS];
    for (size_t i = 0; i < num_fields; i++)
        meta_snapshot[i] = fields[i].m;
    size_t old_size = buf->size;

    struct ppb_lexn_ret ret = ppb_lexn(buf, num_fields, tags, fields, max_lexed_fields);

    check_lexn_postconds(ret, *buf, num_fields, meta_snapshot, fields, old_size, max_lexed_fields, SIZE_MAX,
        data, size);
    return ret;
}

static struct ppb_lexn_ret
do_lexn_hard(struct ppb_buf *buf, size_t limit, size_t num_fields, const struct ppb_encoded_tag *tags,
    struct ppb_field *fields, size_t max_lexed_fields, const uint8_t *data, size_t size)
{
    struct ppb_field_meta meta_snapshot[MAX_FIELDS];
    for (size_t i = 0; i < num_fields; i++)
        meta_snapshot[i] = fields[i].m;
    size_t old_size = buf->size;

    struct ppb_lexn_ret ret = ppb_lexn_with_hard_limit(buf, limit, num_fields, tags, fields,
        max_lexed_fields);

    check_lexn_postconds(ret, *buf, num_fields, meta_snapshot, fields, old_size, max_lexed_fields, limit,
        data, size);
    if (ret.status == PPB_OK)
        POSTCOND(old_size - buf->size <= limit);
    return ret;
}

static struct ppb_lexn_ret
do_lexn_soft(struct ppb_buf *buf, size_t limit, size_t num_fields, const struct ppb_encoded_tag *tags,
    struct ppb_field *fields, size_t max_lexed_fields, const uint8_t *data, size_t size)
{
    struct ppb_field_meta meta_snapshot[MAX_FIELDS];
    for (size_t i = 0; i < num_fields; i++)
        meta_snapshot[i] = fields[i].m;
    size_t old_size = buf->size;

    struct ppb_lexn_ret ret = ppb_lexn_with_soft_limit(buf, limit, num_fields, tags, fields,
        max_lexed_fields);

    check_lexn_postconds(ret, *buf, num_fields, meta_snapshot, fields, old_size, max_lexed_fields, limit,
        data, size);
    return ret;
}

static void
fuzz_lexn_standalone(const uint8_t *data, size_t size, size_t num_fields, const struct ppb_encoded_tag *tags)
{
    struct ppb_field fields[MAX_FIELDS];
    struct ppb_buf buf;

    /* max_lexed_fields = 0: should not advance the buffer. */
    memset(fields, 0, num_fields * sizeof(fields[0]));
    buf = (struct ppb_buf) { data, size };
    do_lexn_call(&buf, num_fields, tags, fields, 0, data, size);
    POSTCOND(buf.size == size);

    memset(fields, 0, num_fields * sizeof(fields[0]));
    buf = (struct ppb_buf) { data, size };
    do_lexn_call(&buf, num_fields, tags, fields, 1, data, size);

    memset(fields, 0, num_fields * sizeof(fields[0]));
    buf = (struct ppb_buf) { data, size };
    do_lexn_call(&buf, num_fields, tags, fields, SIZE_MAX, data, size);

    size_t limits[] = { 0, size / 2, size };
    for (size_t li = 0; li < sizeof(limits) / sizeof(limits[0]); li++)
    {
        memset(fields, 0, num_fields * sizeof(fields[0]));
        buf = (struct ppb_buf) { data, size };
        do_lexn_hard(&buf, limits[li], num_fields, tags, fields, SIZE_MAX, data, size);
    }

    for (size_t li = 0; li < sizeof(limits) / sizeof(limits[0]); li++)
    {
        memset(fields, 0, num_fields * sizeof(fields[0]));
        buf = (struct ppb_buf) { data, size };
        do_lexn_soft(&buf, limits[li], num_fields, tags, fields, SIZE_MAX, data, size);
    }
}

static void
fuzz_prescan_then_lexn(const uint8_t *data, size_t size, size_t num_fields,
    const struct ppb_encoded_tag *tags)
{
    struct ppb_buf buf;
    struct ppb_field fields[MAX_FIELDS];

    buf = (struct ppb_buf) { data, size };
    memset(fields, 0, num_fields * sizeof(fields[0]));

    ptrdiff_t scan = ppb_prescan(buf, num_fields, tags, fields, SIZE_MAX);
    if (scan < 0)
        return; /* invalid input; no lexn */

    buf = (struct ppb_buf) { data, size };
    for (;;)
    {
        if (buf.size == 0)
            break;
        struct ppb_lexn_ret ret = do_lexn_call(&buf, num_fields, tags, fields, SIZE_MAX, data, size);
        if (ret.status != PPB_OK)
            break;
    }

    size_t half = (size / 2 > 0) ? size / 2 : 1;
    buf = (struct ppb_buf) { data, size };
    for (;;)
    {
        if (buf.size == 0)
            break;
        struct ppb_lexn_ret ret = do_lexn_hard(&buf, half, num_fields, tags, fields, SIZE_MAX, data, size);
        if (ret.status != PPB_OK)
            break;
    }

    buf = (struct ppb_buf) { data, size };
    for (;;)
    {
        if (buf.size == 0)
            break;
        struct ppb_lexn_ret ret = do_lexn_soft(&buf, half, num_fields, tags, fields, SIZE_MAX, data, size);
        if (ret.status != PPB_OK)
            break;
    }
}

static void
exercise_config(const uint8_t *data, size_t size, size_t num_fields, const struct ppb_encoded_tag *tags)
{
    struct ppb_field fields[MAX_FIELDS];

    do_prescan(data, size, num_fields, tags, fields);
    fuzz_lexn_standalone(data, size, num_fields, tags);
    fuzz_prescan_then_lexn(data, size, num_fields, tags);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    fuzz_zag(data, size);
    fuzz_decode_varint(data, size);

    exercise_config(data, size, 0, NULL);
    exercise_config(data, size, N_SPECIFIC, SPECIFIC_TAGS);
    exercise_config(data, size, N_CATCHALL, CATCHALL_TAGS);
    exercise_config(data, size, N_MIXED, MIXED_TAGS);

    return 0;
}
