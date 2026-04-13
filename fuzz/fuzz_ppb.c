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
    assert(ppb_validate_tags(N_SPECIFIC, SPECIFIC_TAGS) == PPB_OK);
    assert(ppb_validate_tags(N_CATCHALL, CATCHALL_TAGS) == PPB_OK);
    assert(ppb_validate_tags(N_MIXED, MIXED_TAGS) == PPB_OK);
}

static inline void
check_buf_valid(struct ppb_buf buf, const uint8_t *data, size_t size)
{
    POSTCOND(buf.size <= size);
    POSTCOND((const char *)buf.buf + buf.size == (const char *)data + size);
}

static inline unsigned
wire_type_of(const struct ppb_encoded_tag *tags, size_t idx)
{
    return (unsigned)(tags[idx].bits & 7);
}

/*
 * Accumulate per-field stats from a single lexn-decoded field,
 * for cross-validation against prescan metadata.
 */
static void
accumulate_field(struct ppb_field_meta *acc, const struct ppb_field *fields,
    const struct ppb_encoded_tag *tags, size_t idx)
{
    acc[idx].num_occurrences++;
    if (wire_type_of(tags, idx) == PPB_WIRE_LEN)
    {
        size_t sz = fields[idx].v.payload.size;
        acc[idx].total_bytes += sz;
        if (sz > 0 && (acc[idx].min_nonzero_bytes == 0 || sz < acc[idx].min_nonzero_bytes))
            acc[idx].min_nonzero_bytes = sz;
        if (sz > acc[idx].max_bytes)
            acc[idx].max_bytes = sz;
    }
}

/*
 * Cross-validate prescan metadata against lexn-accumulated per-field
 * stats.  Modeled on the validation in examples/picoscope.c.
 */
static void
cross_validate_meta(size_t num_fields, const struct ppb_encoded_tag *tags,
    const struct ppb_field_meta *prescan_m, const struct ppb_field_meta *lexn_m)
{
    for (size_t i = 0; i < num_fields; i++)
    {
        POSTCOND(prescan_m[i].num_occurrences == lexn_m[i].num_occurrences);

        unsigned wt = wire_type_of(tags, i);

        if (wt == PPB_WIRE_LEN)
        {
            POSTCOND(prescan_m[i].total_bytes == lexn_m[i].total_bytes);
            POSTCOND(prescan_m[i].min_nonzero_bytes == lexn_m[i].min_nonzero_bytes);
            POSTCOND(prescan_m[i].max_bytes == lexn_m[i].max_bytes);
        }

        if (wt == PPB_WIRE_I32)
            POSTCOND(prescan_m[i].total_bytes == 4 * prescan_m[i].num_occurrences);

        if (wt == PPB_WIRE_I64)
            POSTCOND(prescan_m[i].total_bytes == 8 * prescan_m[i].num_occurrences);

        if (wt == PPB_WIRE_VARINT)
        {
            /* At least 1 byte per varint, at most 10. */
            POSTCOND(prescan_m[i].total_bytes >= prescan_m[i].num_occurrences);
            POSTCOND(prescan_m[i].total_bytes <= 10 * prescan_m[i].num_occurrences);
        }
    }
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
    ptrdiff_t full_r;

    {
        memset(fields, 0, num_fields * sizeof(fields[0]));
        full_r = ppb_prescan(buf, num_fields, tags, fields, SIZE_MAX);

        if (full_r >= 0)
        {
            POSTCOND((size_t)full_r <= size);
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

        /* Hard limit. */
        memset(fields, 0, num_fields * sizeof(fields[0]));
        ptrdiff_t hr = ppb_prescan_with_hard_limit(buf, limit, num_fields, tags, fields, SIZE_MAX);

        if (hr >= 0)
        {
            POSTCOND((size_t)hr <= size);
            POSTCOND((size_t)hr <= limit);
            POSTCOND(num_fields == 0 || tags[0].bits > 7);
        }

        /* Save hard-limit metadata for cross-check with soft limit. */
        struct ppb_field_meta hard_m[MAX_FIELDS];
        for (size_t i = 0; i < num_fields; i++)
            hard_m[i] = fields[i].m;

        /* Soft limit with the same limit value. */
        memset(fields, 0, num_fields * sizeof(fields[0]));
        ptrdiff_t sr = ppb_prescan_with_soft_limit(buf, limit, num_fields, tags, fields, SIZE_MAX);

        if (sr >= 0)
        {
            POSTCOND((size_t)sr <= size);
            POSTCOND(num_fields == 0 || tags[0].bits > 7);
        }

        /*
         * Cross-check hard vs soft limit.  Both stop at the first
         * field boundary where consumed >= limit.  Hard signals
         * LIMIT_EXCEEDED when consumed > limit; soft does not.
         */
        if (hr >= 0)
        {
            /* Hard succeeded (consumed <= limit) → soft at same boundary. */
            POSTCOND(sr == hr);
            /* Same fields processed → metadata must match. */
            for (size_t i = 0; i < num_fields; i++)
                POSTCOND(memcmp(&hard_m[i], &fields[i].m, sizeof(hard_m[0])) == 0);
        }
        else if (hr == PPB_ERROR_LIMIT_EXCEEDED)
        {
            /* Field straddled limit → soft succeeds past it. */
            POSTCOND(sr >= 0);
            POSTCOND((size_t)sr > limit);
        }
        else
        {
            /* Data error before reaching limit → both see it. */
            POSTCOND(sr == hr);
        }

        /* Cross-check against unlimited prescan. */
        if (full_r >= 0 && hr >= 0)
            POSTCOND(hr <= full_r);
        if (full_r >= 0 && sr >= 0)
            POSTCOND(sr <= full_r);
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

    /* Save prescan metadata for cross-validation against lexn. */
    struct ppb_field_meta prescan_m[MAX_FIELDS];
    for (size_t i = 0; i < num_fields; i++)
        prescan_m[i] = fields[i].m;

    /*
     * Lex one field at a time, accumulating per-field stats for
     * cross-validation against prescan metadata (cf. picoscope.c).
     */
    struct ppb_field_meta lexn_m[MAX_FIELDS];
    memset(lexn_m, 0, num_fields * sizeof(lexn_m[0]));

    buf = (struct ppb_buf) { data, size };
    while (buf.size > 0)
    {
        const void *old_buf = buf.buf;
        struct ppb_lexn_ret ret = do_lexn_call(&buf, num_fields, tags, fields, 1, data, size);
        if (ret.status != PPB_OK)
            break;

        if (ret.field_range > 0)
        {
            POSTCOND(ret.first_field < num_fields);
            POSTCOND(fields[ret.first_field].v.ptr >= old_buf);
            accumulate_field(lexn_m, fields, tags, ret.first_field);
        }
    }

    /* Prescan validated → lexn must consume entire message. */
    POSTCOND(buf.size == 0);

    /* Prescan metadata must match lexn-accumulated stats. */
    cross_validate_meta(num_fields, tags, prescan_m, lexn_m);

    /*
     * Cross-check hard vs soft limit lexn: run both independently
     * with the same limit, then verify soft consumed >= hard.
     */
    size_t half = (size / 2 > 0) ? size / 2 : 1;

    struct ppb_field hard_fields[MAX_FIELDS];
    memset(hard_fields, 0, num_fields * sizeof(hard_fields[0]));
    buf = (struct ppb_buf) { data, size };
    while (buf.size > 0)
    {
        struct ppb_lexn_ret ret = do_lexn_hard(&buf, half, num_fields, tags, hard_fields, SIZE_MAX, data,
            size);
        if (ret.status != PPB_OK)
            break;
    }
    size_t hard_remaining = buf.size;

    struct ppb_field soft_fields[MAX_FIELDS];
    memset(soft_fields, 0, num_fields * sizeof(soft_fields[0]));
    buf = (struct ppb_buf) { data, size };
    while (buf.size > 0)
    {
        struct ppb_lexn_ret ret = do_lexn_soft(&buf, half, num_fields, tags, soft_fields, SIZE_MAX, data,
            size);
        if (ret.status != PPB_OK)
            break;
    }
    size_t soft_remaining = buf.size;

    /* Soft limit is strictly more permissive → consumes >= hard. */
    POSTCOND(soft_remaining <= hard_remaining);

    /* Valid message + soft limit → must consume everything. */
    POSTCOND(soft_remaining == 0);
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
