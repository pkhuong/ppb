/*
 * ubench.c -- bulk varint-message microbenchmark.
 *
 *   message Outer { repeated Inner items = 42; }
 *   message Inner {
 *     uint64 id      = 1;  // always present; autoincrement from random base
 *     uint64 deleted = 2;  // always absent
 *     uint64 src     = 3;  // always present; uniform in [0, 100_000_000)
 *     uint64 dst     = 4;  // always present; uniform in [0, 100_000_000)
 *   }
 */
#define _POSIX_C_SOURCE 199309L

#include "ppb/ppb.h"

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
https://prng.di.unimi.it/splitmix64.c

Written in 2015 by Sebastiano Vigna (vigna@acm.org)

To the extent possible under law, the author has dedicated all copyright
and related and neighboring rights to this software to the public domain
worldwide.

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR
IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
static uint64_t
splitmix64(uint64_t *state)
{
    uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
    z = (z ^ (z >> 27)) * 0x94d049bb133111eb;
    return z ^ (z >> 31);
}

/*
https://prng.di.unimi.it/xoroshiro128plus.c

Written in 2016-2018 by David Blackman and Sebastiano Vigna (vigna@acm.org)

To the extent possible under law, the author has dedicated all copyright
and related and neighboring rights to this software to the public domain
worldwide.

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR
IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
static inline uint64_t
rotl64(const uint64_t x, int k)
{
    return (x << k) | (x >> (64 - k));
}

static uint64_t
xoro128p_next(uint64_t s[static 2])
{
    const uint64_t s0 = s[0];
    uint64_t s1 = s[1];
    const uint64_t result = s0 + s1;

    s1 ^= s0;
    s[0] = rotl64(s0, 24) ^ s1 ^ (s1 << 16);  // a, b
    s[1] = rotl64(s1, 37);  // c
    return result;
}

static void
seed_prng(uint64_t s[static 2], uint64_t run_index)
{
    uint64_t smx = run_index;
    s[0] = splitmix64(&smx);
    s[1] = splitmix64(&smx);
}

/*
 * Writable counterpart to ppb_buf.  buf advances on each write;
 * overflow is sticky: once set, all subsequent writes are no-ops.
 */
struct ppb_wbuf
{
    uint8_t *buf;   /* current write position */
    uint8_t *end;   /* one past the last writable byte */
    bool overflow;  /* sticky: true if any write was skipped */
};

static void
wbuf_write_byte(struct ppb_wbuf *w, uint8_t byte)
{
    if (w->buf >= w->end)
    {
        w->overflow = true;
        return;
    }
    *w->buf++ = byte;
}

static void
encode_varint(struct ppb_wbuf *w, uint64_t v)
{
    while (v >= 0x80)
    {
        wbuf_write_byte(w, (uint8_t)(v | 0x80));
        v >>= 7;
    }
    wbuf_write_byte(w, (uint8_t)v);
}

static void
encode_field_varint(struct ppb_wbuf *w, uint32_t field_num, uint64_t v)
{
    encode_varint(w, ((uint64_t)field_num << 3) | 0);
    encode_varint(w, v);
}

static void
encode_field_len(struct ppb_wbuf *w, uint32_t field_num, const uint8_t *payload, size_t len)
{
    encode_varint(w, ((uint64_t)field_num << 3) | 2);
    encode_varint(w, (uint64_t)len);
    if ((size_t)(w->end - w->buf) < len)
    {
        w->overflow = true;
        return;
    }
    memcpy(w->buf, payload, len);
    w->buf += len;
}

#define INPUT_TARGET_SIZE  (1024 * 1024UL)    /* stop after exceeding 1 MB */
#define INPUT_BUF_CAPACITY (2 * 1024 * 1024UL)
#define OUTPUT_SW_PIPELINE 2

/*
 * Fills buf with a valid protobuf Outer message (repeated field 42 = Inner).
 * Generates records until the input exceeds INPUT_TARGET_SIZE bytes.
 * Returns the input byte count; sets *msg_count_out to the record count.
 */
static size_t
gen_input(uint8_t *buf, size_t buf_size, uint64_t prng[2], size_t *msg_count_out)
{
    uint64_t base_id = xoro128p_next(prng) % (10 * 1000 * 1000ULL);
    struct ppb_wbuf outer = { buf, buf + buf_size, false };
    size_t count = 0;

    for (;;)
    {
        /* encode Inner message into a stack buffer */
        uint8_t inner_storage[64];
        struct ppb_wbuf inner = { inner_storage, inner_storage + sizeof(inner_storage), false };
        encode_field_varint(&inner, 1, base_id + count);
        encode_field_varint(&inner, 3, xoro128p_next(prng) % (100 * 1000 * 1000ULL));
        encode_field_varint(&inner, 4, xoro128p_next(prng) % (100 * 1000 * 1000ULL));
        if (inner.overflow)
        {
            /* inner_storage[64] is too small -- should never happen */
            fprintf(stderr, "inner buffer overflow (bug)\n");
            abort();
        }

        size_t inner_len = (size_t)(inner.buf - inner_storage);
        uint8_t *checkpoint = outer.buf;
        encode_field_len(&outer, 42, inner_storage, inner_len);
        if (outer.overflow)
        {
            outer.buf = checkpoint;
            break;
        }

        count++;
        if ((size_t)(outer.buf - buf) > INPUT_TARGET_SIZE)
        {
            break;
        }
    }

    *msg_count_out = count;
    return (size_t)(outer.buf - buf);
}

/*
 * Decodes the Outer message in [input, input+input_size).
 * Returns the number of messages decoded.
 */
static const struct ppb_encoded_tag outer_tags[1] = { PPB_TAG(42, PPB_WIRE_LEN) };
static const struct ppb_encoded_tag inner_tags[4] = {
    PPB_TAG(1, PPB_WIRE_VARINT),
    PPB_TAG(2, PPB_WIRE_VARINT),
    PPB_TAG(3, PPB_WIRE_VARINT),
    PPB_TAG(4, PPB_WIRE_VARINT),
};

static size_t
decode_input(const uint8_t *input, size_t input_size)
{
    struct ppb_field outer_fields[1] = { 0 };
    struct ppb_field inner_fields[OUTPUT_SW_PIPELINE][4] = { 0 };
    struct ppb_buf buf = { input, input_size };
    const char *end_of_input = (const char *)input + input_size;
    size_t count = 0;

    while (buf.size > 0)
    {
        struct ppb_lexn_ret ret = ppb_lexn(&buf, 1, outer_tags, outer_fields, 1);
        if (ret.status != PPB_OK)
        {
            fprintf(stderr, "ppb_lexn error %d at message %zu\n", (int)ret.status, count);
            exit(1);
        }

        {
            struct ppb_buf payload = outer_fields[0].v.payload;
            struct ppb_field *dst = inner_fields[count % OUTPUT_SW_PIPELINE];

            /* reset: absent fields default to 0 */
            dst[0].v.u64 = 0;
            dst[1].v.u64 = 0;
            dst[2].v.u64 = 0;
            dst[3].v.u64 = 0;

            /*
             * Prescan the inner message over the wider slice [payload.buf,
             * end_of_input) with a hard limit of payload.size: a record
             * that claims to be smaller than it really is is caught here
             * rather than silently reading past its declared boundary.
             */
            struct ppb_buf wider = {
                .buf = payload.buf,
                .size = (size_t)(end_of_input - (const char *)payload.buf),
            };

            ppb_prescan_with_hard_limit(wider, payload.size, 4, inner_tags, dst, SIZE_MAX);
        }

        const struct ppb_field *result = inner_fields[(count + 1) % OUTPUT_SW_PIPELINE];
        uint64_t id = result[0].v.u64;
        uint64_t deleted = result[1].v.u64;
        uint64_t src = result[2].v.u64;
        uint64_t dst = result[3].v.u64;

        /* prevent the compiler from eliminating the decode (on 64-bit arch, 32b is slow anyway) */
        if (sizeof(long) >= sizeof(uint64_t))
        {
            __asm__ volatile("" : : "r"(id), "r"(deleted), "r"(src), "r"(dst));
        }

        count++;
    }

    return count;
}

struct run_sample
{
    double ns_per_msg;
    double mb_per_s;
};

static int
cmp_sample_ns(const void *a, const void *b)
{
    double da = ((const struct run_sample *)a)->ns_per_msg;
    double db = ((const struct run_sample *)b)->ns_per_msg;
    return (da > db) - (da < db);
}

#define NWARM 100
#define NREP  1000

int
main(void)
{
    assert(ppb_validate_tags(1, outer_tags) == PPB_OK);
    assert(ppb_validate_tags(4, inner_tags) == PPB_OK);

    uint8_t *input_buf = malloc(INPUT_BUF_CAPACITY);
    if (!input_buf)
    {
        perror("malloc");
        return 1;
    }

    struct run_sample samples[NREP];
    size_t last_input_size = 0;
    size_t last_msg_count = 0;

    for (int run = 0; run < NWARM + NREP; run++)
    {
        uint64_t prng[2];
        seed_prng(prng, (uint64_t)run);

        size_t msg_count = 0;
        size_t input_size = gen_input(input_buf, INPUT_BUF_CAPACITY, prng, &msg_count);

        struct timespec ts_start, ts_end;
        clock_gettime(CLOCK_MONOTONIC, &ts_start);
        size_t decoded = decode_input(input_buf, input_size);
        clock_gettime(CLOCK_MONOTONIC, &ts_end);

        uint64_t elapsed_ns = ((uint64_t)ts_end.tv_sec * 1000 * 1000 * 1000ULL + (uint64_t)ts_end.tv_nsec) -
            ((uint64_t)ts_start.tv_sec * 1000 * 1000 * 1000ULL + (uint64_t)ts_start.tv_nsec);

        if (run < NWARM)
        {
            continue;
        }

        int idx = run - NWARM;
        samples[idx].ns_per_msg = (double)elapsed_ns / fmax(1.0, (double)decoded);
        samples[idx].mb_per_s = (1e-6 * (double)input_size) / (1e-9 * fmax(1.0, (double)elapsed_ns));

        last_input_size = input_size;
        last_msg_count = msg_count;
    }

    qsort(samples, NREP, sizeof(samples[0]), cmp_sample_ns);

    /* samples sorted ascending by ns_per_msg; p5 ns/msg == p95 MB/s */
    size_t p5 = 5 * NREP / 100;
    size_t p50 = 50 * NREP / 100;
    size_t p95 = 95 * NREP / 100;

    printf("input: %zu messages, %zu bytes\n", last_msg_count, last_input_size);
    printf("ns/msg:  p5=%.0f  median=%.0f  p95=%.0f\n", samples[p5].ns_per_msg, samples[p50].ns_per_msg,
        samples[p95].ns_per_msg);
    /* not quite right, but close enough. */
    printf("MB/s:    p5=%.0f  median=%.0f  p95=%.0f\n", samples[p95].mb_per_s, samples[p50].mb_per_s,
        samples[p5].mb_per_s);

    free(input_buf);
    return 0;
}
