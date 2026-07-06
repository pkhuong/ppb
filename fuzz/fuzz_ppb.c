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
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
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

/* Catch-all only: one entry per wire type, field -1 (= UINT64_MAX). */
static const struct ppb_encoded_tag CATCHALL_TAGS[] = {
    PPB_TAG(-1, PPB_WIRE_VARINT),
    PPB_TAG(-1, PPB_WIRE_I64),
    PPB_TAG(-1, PPB_WIRE_LEN),
    PPB_TAG(-1, PPB_WIRE_I32),
};
#define N_CATCHALL 4

/* Mixed: two specific fields + four catch-alls. */
static const struct ppb_encoded_tag MIXED_TAGS[] = {
    PPB_TAG(1, PPB_WIRE_VARINT),
    PPB_TAG(3, PPB_WIRE_LEN),
    PPB_TAG(-1, PPB_WIRE_VARINT),
    PPB_TAG(-1, PPB_WIRE_I64),
    PPB_TAG(-1, PPB_WIRE_LEN),
    PPB_TAG(-1, PPB_WIRE_I32),
};
#define N_MIXED 6

/*
 * Large field numbers: one specific entry per wire type at a 2- or 3-byte
 * tag, plus partial catch-alls (VARINT and LEN only).  Exercises both
 * multi-byte tag encoding paths and the "no catch-all for this wire type"
 * skip path (I64 and I32 unknowns are silently skipped).
 *
 *   field 100,   VARINT  ->  2-byte tag  (tag varint = 800)
 *   field 500,   I64     ->  2-byte tag  (tag varint = 4001)
 *   field 10000, LEN     ->  3-byte tag  (tag varint = 80002)
 *   field 20000, I32     ->  3-byte tag  (tag varint = 160005)
 */
static const struct ppb_encoded_tag LARGE_TAGS[] = {
    PPB_TAG(100, PPB_WIRE_VARINT),   /* 2-byte tag */
    PPB_TAG(500, PPB_WIRE_I64),      /* 2-byte tag */
    PPB_TAG(10000, PPB_WIRE_LEN),    /* 3-byte tag */
    PPB_TAG(20000, PPB_WIRE_I32),    /* 3-byte tag */
    PPB_TAG(-1, PPB_WIRE_VARINT),    /* catch-all  */
    PPB_TAG(-1, PPB_WIRE_LEN),      /* catch-all  */
};
#define N_LARGE 6

/* Maximum field arrays (sized to the largest configuration). */
#define MAX_FIELDS N_LARGE

/* Validate all tag arrays once, before any fuzzer input is processed. */
__attribute__((constructor)) static void
check_tag_ordering(void)
{
    assert(ppb_validate_tags(/*num_fields=*/N_SPECIFIC, SPECIFIC_TAGS) == PPB_OK);
    assert(ppb_validate_tags(/*num_fields=*/N_CATCHALL, CATCHALL_TAGS) == PPB_OK);
    assert(ppb_validate_tags(/*num_fields=*/N_MIXED, MIXED_TAGS) == PPB_OK);
    assert(ppb_validate_tags(/*num_fields=*/N_LARGE, LARGE_TAGS) == PPB_OK);
}

/*
 * Allocates a fields[] array of exactly `num_fields` elements so
 * ASan's heap redzones flag any write past the last field.  This is
 * tighter that a fixed-size stack array, which could silently absorb
 * off-by-ones with its unused tail, when oversized.
 */
static struct ppb_field *
alloc_fields(size_t num_fields)
{
    struct ppb_field *fields = calloc(num_fields != 0 ? num_fields : 1, sizeof(*fields));
    if (fields == NULL)
        abort();

    return fields;
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
 * Verify that every field decoded by ppb_prescan has its ptr and payload
 * within the scanned slice [data, scan_end).  `fields[]` is always freshly
 * zeroed before each prescan call, so any non-NULL ptr was set by prescan.
 */
static inline void
check_prescan_fields_in_range(const uint8_t *data, const uint8_t *scan_end, size_t num_fields,
    const struct ppb_encoded_tag *tags, const struct ppb_field *fields)
{
    for (size_t i = 0; i < num_fields; i++)
    {
        const struct ppb_field_value *v = &fields[i].v;
        if (v->ptr == NULL)
            continue;

        POSTCOND((const uint8_t *)v->ptr >= data);
        POSTCOND((const uint8_t *)v->ptr < scan_end);
        if (wire_type_of(tags, i) == PPB_WIRE_LEN)
        {
            POSTCOND((const uint8_t *)v->payload.buf > (const uint8_t *)v->ptr);
            POSTCOND((const uint8_t *)v->payload.buf >= data);
            POSTCOND((const uint8_t *)v->payload.buf + v->payload.size <= scan_end);
        }
    }
}

/*
 * Verify that every field decoded in the current ppb_lexn* call has its
 * payload within the consumed slice `[buf_before, buf_after)`.
 *
 * `buf_before = buf->buf` before the call; `buf_after = buf->buf after`.
 */
static inline void
check_lexn_fields_in_range(const void *buf_before, const void *buf_after, size_t num_fields,
    const struct ppb_encoded_tag *tags, const struct ppb_field *fields)
{
    for (size_t i = 0; i < num_fields; i++)
    {
        const struct ppb_field_value *v = &fields[i].v;
        if (v->ptr == NULL || (const uint8_t *)v->ptr < (const uint8_t *)buf_before ||
            (const uint8_t *)v->ptr >= (const uint8_t *)buf_after)
            continue;

        /* ptr in [buf_before, buf_after) — field was decoded in this call. */
        if (wire_type_of(tags, i) == PPB_WIRE_LEN)
        {
            POSTCOND((const uint8_t *)v->payload.buf > (const uint8_t *)v->ptr);
            POSTCOND((const uint8_t *)v->payload.buf >= (const uint8_t *)buf_before);
            POSTCOND((const uint8_t *)v->payload.buf + v->payload.size <= (const uint8_t *)buf_after);
        }
    }
}

/*
 * Accumulate per-field stats from a single lexn-decoded field,
 * for cross-validation against prescan metadata.
 *
 * `prev_u64` tracks the previous occurrence's u64 (one entry per
 * field index) so we can recompute `lost_distinct_u64` on the lexn
 * side and compare against prescan's value, mirroring handle_field.
 */
static void
accumulate_field(struct ppb_field_meta *acc, uint64_t *prev_u64, const struct ppb_field *fields,
    const struct ppb_encoded_tag *tags, size_t idx)
{
    bool first = acc[idx].num_occurrences == 0;
    if (wire_type_of(tags, idx) == PPB_WIRE_LEN)
    {
        size_t sz = fields[idx].v.payload.size;
        acc[idx].total_bytes += sz;
        if (sz > 0 && (acc[idx].min_nonzero_bytes == 0 || sz < acc[idx].min_nonzero_bytes))
            acc[idx].min_nonzero_bytes = sz;
        if (sz > acc[idx].max_bytes)
            acc[idx].max_bytes = sz;
    }
    else
    {
        uint64_t v = fields[idx].v.u64;

        if (!first && v != prev_u64[idx])
            acc[idx].lost_distinct_u64 = 1;

        prev_u64[idx] = v;
    }

    acc[idx].num_occurrences++;
}

/* Cross-validate prescan metadata against lexn-accumulated per-field stats. */
static void
cross_validate_meta(size_t num_fields, const struct ppb_encoded_tag *tags,
    const struct ppb_field_meta *prescan_m, const struct ppb_field_meta *lexn_m)
{
    for (size_t i = 0; i < num_fields; i++)
    {
        POSTCOND(prescan_m[i].num_occurrences == lexn_m[i].num_occurrences);
        POSTCOND(prescan_m[i].lost_distinct_u64 == lexn_m[i].lost_distinct_u64);

        unsigned wt = wire_type_of(tags, i);

        if (wt == PPB_WIRE_LEN)
        {
            POSTCOND(prescan_m[i].total_bytes == lexn_m[i].total_bytes);
            POSTCOND(prescan_m[i].min_nonzero_bytes == lexn_m[i].min_nonzero_bytes);
            POSTCOND(prescan_m[i].max_bytes == lexn_m[i].max_bytes);
            /* LEN never sets the flag, regardless of length variation. */
            POSTCOND(prescan_m[i].lost_distinct_u64 == 0);
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
    size_t n = size < sizeof(x) ? size : sizeof(x);
    memcpy(&x, data, n);

    for (size_t rep = 0; rep < 2; rep++)
    {
        int64_t r = ppb_zag(x);
        int32_t r32 = ppb_zag32((uint32_t)x);

        /* Admitted postcondition: sint32 behavior of ppb_zag. */
        if (x < (1UL << 32))
        {
            POSTCOND(r >= (int64_t)INT32_MIN);
            POSTCOND(r <= (int64_t)INT32_MAX);
            POSTCOND(r == r32);
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
        full_r = ppb_prescan(buf, num_fields, tags, fields, /*max_lexed_fields=*/SIZE_MAX);

        if (full_r >= 0)
        {
            POSTCOND((size_t)full_r <= size);
            POSTCOND(num_fields == 0 || tags[0].bits > 7);
        }

        check_prescan_fields_in_range(data, data + (full_r >= 0 ? (size_t)full_r : size), num_fields, tags,
            fields);
    }

    {
        memset(fields, 0, num_fields * sizeof(fields[0]));
        ptrdiff_t r = ppb_prescan(buf, num_fields, tags, fields, /*max_lexed_fields=*/0);

        if (r >= 0)
        {
            POSTCOND((size_t)r <= size);
        }

        check_prescan_fields_in_range(data, data + (r >= 0 ? (size_t)r : size), num_fields, tags, fields);
    }

    size_t limits[] = { 0, size / 2, size, SIZE_MAX };
    for (size_t li = 0; li < sizeof(limits) / sizeof(limits[0]); li++)
    {
        size_t limit = limits[li];

        /* Hard limit. */
        memset(fields, 0, num_fields * sizeof(fields[0]));
        ptrdiff_t hr = ppb_prescan_with_hard_limit(buf, limit, num_fields, tags, fields,
            /*max_lexed_fields=*/SIZE_MAX);

        if (hr >= 0)
        {
            POSTCOND((size_t)hr <= size);
            POSTCOND((size_t)hr <= limit);
            POSTCOND(num_fields == 0 || tags[0].bits > 7);
        }

        check_prescan_fields_in_range(data, data + (hr >= 0 ? (size_t)hr : size), num_fields, tags, fields);

        struct ppb_field_meta hard_m[MAX_FIELDS];
        for (size_t i = 0; i < num_fields; i++)
            hard_m[i] = fields[i].m;

        /* Soft limit with the same byte budget. */
        memset(fields, 0, num_fields * sizeof(fields[0]));
        ptrdiff_t sr = ppb_prescan_with_soft_limit(buf, limit, num_fields, tags, fields,
            /*max_lexed_fields=*/SIZE_MAX);

        if (sr >= 0)
        {
            POSTCOND((size_t)sr <= size);
            POSTCOND(num_fields == 0 || tags[0].bits > 7);
        }

        check_prescan_fields_in_range(data, data + (sr >= 0 ? (size_t)sr : size), num_fields, tags, fields);

        /*
         * Cross-check hard vs soft limit: hard returns LIMIT_EXCEEDED
         * when a field straddles the budget; soft silently continues.
         */
        if (hr >= 0)
        {
            POSTCOND(sr == hr);
            for (size_t i = 0; i < num_fields; i++)
                POSTCOND(memcmp(&hard_m[i], &fields[i].m, sizeof(hard_m[0])) == 0);
        }
        else if (hr == PPB_ERROR_LIMIT_EXCEEDED)
        {
            POSTCOND(sr >= 0);
            POSTCOND((size_t)sr > limit);
        }
        else
        {
            POSTCOND(sr == hr);
        }

        /* Both limited prescans must consume <= the unlimited one. */
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
    const void *buf_before = buf->buf;

    struct ppb_lexn_ret ret = ppb_lexn(buf, num_fields, tags, fields, max_lexed_fields);

    check_lexn_postconds(ret, *buf, num_fields, meta_snapshot, fields, old_size, max_lexed_fields,
        /*limit=*/SIZE_MAX, data, /*buf_size=*/size);
    check_lexn_fields_in_range(buf_before, buf->buf, num_fields, tags, fields);
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
    const void *buf_before = buf->buf;

    struct ppb_lexn_ret ret = ppb_lexn_with_hard_limit(buf, /*limit=*/limit, num_fields, tags, fields,
        max_lexed_fields);

    check_lexn_postconds(ret, *buf, num_fields, meta_snapshot, fields, old_size, max_lexed_fields,
        /*limit=*/limit, data, /*buf_size=*/size);
    if (ret.status == PPB_OK)
        POSTCOND(old_size - buf->size <= limit);

    check_lexn_fields_in_range(buf_before, buf->buf, num_fields, tags, fields);
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
    const void *buf_before = buf->buf;

    struct ppb_lexn_ret ret = ppb_lexn_with_soft_limit(buf, /*limit=*/limit, num_fields, tags, fields,
        max_lexed_fields);

    check_lexn_postconds(ret, *buf, num_fields, meta_snapshot, fields, old_size, max_lexed_fields,
        /*limit=*/limit, data, /*buf_size=*/size);
    check_lexn_fields_in_range(buf_before, buf->buf, num_fields, tags, fields);
    return ret;
}

static void
fuzz_lexn_standalone(const uint8_t *data, size_t size, size_t num_fields, const struct ppb_encoded_tag *tags)
{
    struct ppb_field *fields = alloc_fields(num_fields);
    struct ppb_buf buf;

    /* max_lexed_fields = 0: should not advance the buffer. */
    memset(fields, 0, num_fields * sizeof(fields[0]));
    buf = (struct ppb_buf) { data, size };
    do_lexn_call(&buf, num_fields, tags, fields,
        /*max_lexed_fields=*/0, data, /*buf_size=*/size);
    POSTCOND(buf.size == size);

    memset(fields, 0, num_fields * sizeof(fields[0]));
    buf = (struct ppb_buf) { data, size };
    do_lexn_call(&buf, num_fields, tags, fields,
        /*max_lexed_fields=*/1, data, /*buf_size=*/size);

    memset(fields, 0, num_fields * sizeof(fields[0]));
    buf = (struct ppb_buf) { data, size };
    do_lexn_call(&buf, num_fields, tags, fields,
        /*max_lexed_fields=*/SIZE_MAX, data, /*buf_size=*/size);

    size_t limits[] = { 0, size / 2, size };
    for (size_t li = 0; li < sizeof(limits) / sizeof(limits[0]); li++)
    {
        memset(fields, 0, num_fields * sizeof(fields[0]));
        buf = (struct ppb_buf) { data, size };
        do_lexn_hard(&buf, /*limit=*/limits[li], num_fields, tags, fields,
            /*max_lexed_fields=*/SIZE_MAX, data, /*buf_size=*/size);
    }

    for (size_t li = 0; li < sizeof(limits) / sizeof(limits[0]); li++)
    {
        memset(fields, 0, num_fields * sizeof(fields[0]));
        buf = (struct ppb_buf) { data, size };
        do_lexn_soft(&buf, /*limit=*/limits[li], num_fields, tags, fields,
            /*max_lexed_fields=*/SIZE_MAX, data, /*buf_size=*/size);
    }

    free(fields);
}

static void
fuzz_prescan_then_lexn(const uint8_t *data, size_t size, size_t num_fields,
    const struct ppb_encoded_tag *tags)
{
    struct ppb_buf buf;
    struct ppb_field *fields = alloc_fields(num_fields);

    buf = (struct ppb_buf) { data, size };
    memset(fields, 0, num_fields * sizeof(fields[0]));

    ptrdiff_t scan = ppb_prescan(buf, num_fields, tags, fields, /*max_lexed_fields=*/SIZE_MAX);
    if (scan < 0)
    {
        /* invalid input; no lexn */
        free(fields);
        return;
    }

    struct ppb_field_meta prescan_m[MAX_FIELDS];
    for (size_t i = 0; i < num_fields; i++)
        prescan_m[i] = fields[i].m;

    /* Lex one field at a time, accumulating stats for cross-validation. */
    struct ppb_field_meta lexn_m[MAX_FIELDS];
    memset(lexn_m, 0, num_fields * sizeof(lexn_m[0]));
    uint64_t lexn_prev_u64[MAX_FIELDS] = { 0 };

    buf = (struct ppb_buf) { data, size };
    while (buf.size > 0)
    {
        const void *old_buf = buf.buf;
        struct ppb_lexn_ret ret = do_lexn_call(&buf, num_fields, tags, fields,
            /*max_lexed_fields=*/1, data, /*buf_size=*/size);
        if (ret.status != PPB_OK)
            break;

        if (ret.field_range > 0)
        {
            POSTCOND(ret.first_field < num_fields);
            POSTCOND(fields[ret.first_field].v.ptr >= old_buf);
            accumulate_field(lexn_m, lexn_prev_u64, fields, tags, ret.first_field);
        }
    }

    POSTCOND(buf.size == 0); /* prescan validated, so lexn must consume all */
    cross_validate_meta(num_fields, tags, prescan_m, lexn_m);

    /* Cross-check hard vs soft limit lexn. */
    size_t half = (size / 2 > 0) ? size / 2 : 1;

    struct ppb_field *hard_fields = alloc_fields(num_fields);
    memset(hard_fields, 0, num_fields * sizeof(hard_fields[0]));
    buf = (struct ppb_buf) { data, size };
    while (buf.size > 0)
    {
        struct ppb_lexn_ret ret = do_lexn_hard(&buf, /*limit=*/half, num_fields, tags, hard_fields,
            /*max_lexed_fields=*/SIZE_MAX, data, /*buf_size=*/size);
        if (ret.status != PPB_OK)
            break;
    }
    size_t hard_remaining = buf.size;

    struct ppb_field *soft_fields = alloc_fields(num_fields);
    memset(soft_fields, 0, num_fields * sizeof(soft_fields[0]));
    buf = (struct ppb_buf) { data, size };
    while (buf.size > 0)
    {
        struct ppb_lexn_ret ret = do_lexn_soft(&buf, /*limit=*/half, num_fields, tags, soft_fields,
            /*max_lexed_fields=*/SIZE_MAX, data, /*buf_size=*/size);
        if (ret.status != PPB_OK)
            break;
    }
    size_t soft_remaining = buf.size;

    POSTCOND(soft_remaining <= hard_remaining); /* soft is more permissive */
    POSTCOND(soft_remaining == 0);             /* valid, so fully consumed  */

    /*
     * For each ppb_lexn call, any catch-all field (tag.bits < 0) is
     * always the last decoded field in the batch.
     */
    memset(fields, 0, num_fields * sizeof(fields[0]));
    buf = (struct ppb_buf) { data, size };
    while (buf.size > 0)
    {
        const void *old_ptr = buf.buf;
        struct ppb_lexn_ret ret = do_lexn_call(&buf, num_fields, tags, fields,
            /*max_lexed_fields=*/SIZE_MAX, data, /*buf_size=*/size);
        if (ret.status != PPB_OK)
            break;
        if (ret.field_range == 0)
            continue;

        /*
         * Scan decoded fields in index order: once we see a catch-all,
         * no higher-indexed field in this batch may have been decoded.
         */
        size_t end = ret.first_field + (size_t)ret.field_range;

        bool past_catchall = false;
        for (size_t j = ret.first_field; j < end; j++)
        {
            bool decoded_here = (const void *)fields[j].v.ptr >= old_ptr;
            if (past_catchall)
                POSTCOND(!decoded_here);
            if (decoded_here && (int64_t)tags[j].bits < 0)
                past_catchall = true;
        }
    }

    POSTCOND(buf.size == 0);

    free(soft_fields);
    free(hard_fields);
    free(fields);
}

/*
 * Exercise ppb_validate_tags, ppb_prescan, and ppb_lexn with an
 * arbitrary (potentially unsorted or invalid) tag array derived from
 * the fuzz input.
 *
 * There's no guarantee on the result when the tags array is invalid,
 * but there shouldn't be any UB.
 */
static void
fuzz_invalid_tags(const uint8_t *data, size_t size)
{
    if (size < 1)
        return;

    size_t num_tags = (size_t)(data[0] % (MAX_FIELDS + 1));
    size_t header = 1 + num_tags * sizeof(struct ppb_encoded_tag);
    if (size < header)
        return;

    /* Heap-allocate tags[] and fields[] at exactly num_tags elements for ASan. */
    struct ppb_encoded_tag *tags = num_tags ? malloc(num_tags * sizeof(*tags)) : NULL;
    struct ppb_field *fields = num_tags ? malloc(num_tags * sizeof(*fields)) : NULL;

    if (num_tags && (tags == NULL || fields == NULL))
    {
        free(tags);
        free(fields);
        return;
    }

    if (num_tags)
    {
        memcpy(tags, data + 1, num_tags * sizeof(*tags));
    }

    const uint8_t *msg = data + header;
    size_t msg_size = size - header;
    struct ppb_buf buf;

    /* Deliberately ignore the result: an invalid tags[] must stay UB-free. */
    ppb_validate_tags(num_tags, tags);

    if (num_tags)
    {
        memset(fields, 0, num_tags * sizeof(*fields));
    }

    buf = (struct ppb_buf) { msg, msg_size };
    ptrdiff_t pr = ppb_prescan(buf, num_tags, tags, fields, SIZE_MAX);
    if (pr >= 0)
        POSTCOND((size_t)pr <= msg_size);

    if (num_tags)
    {
        memset(fields, 0, num_tags * sizeof(*fields));
    }

    buf = (struct ppb_buf) { msg, msg_size };
    while (buf.size > 0)
    {
        size_t initial_size = buf.size;
        struct ppb_lexn_ret ret = ppb_lexn(&buf, num_tags, tags, fields, SIZE_MAX);

        check_buf_valid(buf, msg, msg_size);
        POSTCOND(ret.first_field + ret.field_range <= num_tags);
        if (ret.status != PPB_OK)
            break;

        /* must make progress */
        POSTCOND(buf.size < initial_size);
    }

    free(fields);
    free(tags);
}

static void
exercise_config(const uint8_t *data, size_t size, size_t num_fields, const struct ppb_encoded_tag *tags)
{
    struct ppb_field *fields = alloc_fields(num_fields);

    do_prescan(data, size, num_fields, tags, fields);
    fuzz_lexn_standalone(data, size, num_fields, tags);
    fuzz_prescan_then_lexn(data, size, num_fields, tags);

    free(fields);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    fuzz_zag(data, size);
    fuzz_decode_varint(data, size);
    fuzz_invalid_tags(data, size);

    exercise_config(data, size, /*num_fields=*/0, /*tags=*/NULL);
    exercise_config(data, size, /*num_fields=*/N_SPECIFIC, SPECIFIC_TAGS);
    exercise_config(data, size, /*num_fields=*/N_CATCHALL, CATCHALL_TAGS);
    exercise_config(data, size, /*num_fields=*/N_MIXED, MIXED_TAGS);
    exercise_config(data, size, /*num_fields=*/N_LARGE, LARGE_TAGS);

    return 0;
}
