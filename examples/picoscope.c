#include "ppb/ppb.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Indices into the catch-all fields array, matching the sorted
 * order of PPB_TAG(-1, wire_type).
 */
enum
{
    FIELD_VARINT = 0,  /* PPB_WIRE_VARINT = 0 */
    FIELD_I64 = 1,  /* PPB_WIRE_I64    = 1 */
    FIELD_LEN = 2,  /* PPB_WIRE_LEN    = 2 */
    FIELD_I32 = 3,  /* PPB_WIRE_I32    = 5 */
    NUM_FIELDS = 4,
};

static int disassemble(struct ppb_buf buf, size_t indent);
static int format_field(size_t idx, uint64_t field_num, struct ppb_field_value *v, size_t indent,
    bool newline);

static bool g_compat_mode = false;  /* -p: protoscope-compatible output */

static void
print_indent(size_t indent)
{
    for (size_t i = 0; i < indent; i++)
    {
        putchar(' ');
    }
}

/*
 * Returns the number of top-level fields if payload looks like a
 * valid protobuf submessage (prescan consumes all bytes), 0 otherwise.
 */
static size_t
submessage_field_count(const struct ppb_buf payload)
{
    struct ppb_field fields[NUM_FIELDS] = {
        { .tag = PPB_TAG(-1, PPB_WIRE_VARINT) },
        { .tag = PPB_TAG(-1, PPB_WIRE_I64) },
        { .tag = PPB_TAG(-1, PPB_WIRE_LEN) },
        { .tag = PPB_TAG(-1, PPB_WIRE_I32) },
    };

    if (payload.size == 0)
    {
        return 0;
    }

    ptrdiff_t scanned = ppb_prescan(payload, NUM_FIELDS, fields, SIZE_MAX);
    if (scanned < 0 || (size_t)scanned != payload.size)
    {
        return 0;
    }

    size_t total = 0;
    for (size_t i = 0; i < NUM_FIELDS; i++)
    {
        total += fields[i].m.num_occurrences;
    }

    return total;
}

/*
 * Returns whether payload is a valid sequence of 2+ varints (at least
 * one of which is multi-byte) that fully consumes the buffer.
 */
static bool
looks_like_packed_varints(const struct ppb_buf payload)
{
    if (payload.size == 0)
    {
        return false;
    }

    struct ppb_buf copy = payload;
    size_t count = 0;
    enum ppb_error err = PPB_OK;
    bool has_multibyte = false;

    while (copy.size > 0)
    {
        const char *start = copy.buf;

        ppb_decode_varint(&copy, &err);
        if (err != PPB_OK)
        {
            return false;
        }

        if ((const char *)copy.buf - start > 1)
        {
            has_multibyte = true;
        }

        count++;
    }

    return count >= 2 && has_multibyte;
}

/*
 * Returns whether payload is valid UTF-8 containing only printable
 * characters, tabs, and newlines.
 */
static bool
looks_like_string(const struct ppb_buf payload)
{
    if (payload.size == 0)
    {
        return false;
    }

    const char *p = payload.buf;
    size_t i = 0;

    while (i < payload.size)
    {
        unsigned char c = p[i];

        if (c == '\t' || c == '\r' || c == '\n')
        {
            i++;
            continue;
        }

        /* ASCII printable. */
        if (c >= 0x20 && c <= 0x7e)
        {
            i++;
            continue;
        }

        /* Reject other ASCII control characters. */
        if (c < 0x80)
        {
            return false;
        }

        /* Multi-byte UTF-8: determine expected length. */
        size_t len;
        uint32_t codepoint;
        if ((c & 0xe0) == 0xc0)
        {
            len = 2;
            codepoint = c & 0x1f;
        }
        else if ((c & 0xf0) == 0xe0)
        {
            len = 3;
            codepoint = c & 0x0f;
        }
        else if ((c & 0xf8) == 0xf0)
        {
            len = 4;
            codepoint = c & 0x07;
        }
        else
        {
            return false;
        }

        if (i + len > payload.size)
        {
            return false;
        }

    /* tack on continuation bytes. */
        for (size_t j = 1; j < len; j++)
        {
            if ((p[i + j] & 0xc0) != 0x80)
            {
                return false;
            }

            codepoint = (codepoint << 6) | (p[i + j] & 0x3f);
        }

        /* Reject overlong encodings. */
        if (len == 2 && codepoint < 0x80)
        {
            return false;
        }

        if (len == 3 && codepoint < 0x800)
        {
            return false;
        }

        if (len == 4 && codepoint < 0x10000)
        {
            return false;
        }

        /* Reject surrogates and beyond Unicode max. */
        if (codepoint >= 0xd800 && codepoint <= 0xdfff)
        {
            return false;
        }

        if (codepoint > 0x10ffff)
        {
            return false;
        }

        i += len;
    }

    return true;
}

static void
print_escaped_string(const struct ppb_buf payload)
{
    const char *p = payload.buf;
    for (size_t i = 0; i < payload.size; i++)
    {
        unsigned char c = p[i];
        if (c == '"')
        {
            printf("\\\"");
        }
        else if (c == '\\')
        {
            printf("\\\\");
        }
        else if (c == '\n')
        {
            printf("\\n");
        }
        else if (c >= 0x20 && c <= 0x7e)
        {
            putchar(c);
        }
        else if (c >= 0x80)
        {
            putchar(c);  /* valid UTF-8 (caller already validated) */
        }
        else
        {
            printf("\\x%02x", c);
        }
    }
}

static void
print_hex(const struct ppb_buf payload)
{
    const char *p = payload.buf;
    for (size_t i = 0; i < payload.size; i++)
    {
        printf("%02x", p[i]);
    }
}

static int
format_len_field(uint64_t field_num, const struct ppb_field_value *v, size_t indent, bool newline)
{
    struct ppb_buf payload = v->payload;
    const char *nl = newline ? "\n" : "";

    /* 0. Empty payload. */
    if (payload.size == 0)
    {
        printf("%" PRIu64 ": {}%s", field_num, nl);
        return 0;
    }

    /* 1. Try submessage. */
    size_t nfields = submessage_field_count(payload);
    if (nfields > 0)
    {
        /*
         * In compat mode, inline single-field submessages
         * to match protoscope (e.g., "2: {1: 7}").
         */
        if (g_compat_mode && nfields == 1)
        {
            struct ppb_field inner[NUM_FIELDS] = {
                { .tag = PPB_TAG(-1, PPB_WIRE_VARINT) },
                { .tag = PPB_TAG(-1, PPB_WIRE_I64) },
                { .tag = PPB_TAG(-1, PPB_WIRE_LEN) },
                { .tag = PPB_TAG(-1, PPB_WIRE_I32) },
            };
            struct ppb_buf copy = payload;
            struct ppb_lexn_ret ret = ppb_lexn(&copy, NUM_FIELDS, inner, 1);

            if (ret.status == PPB_OK && ret.field_range > 0)
            {
                size_t iidx = ret.first_field;
                struct ppb_field_value *iv = &inner[iidx].v;
                struct ppb_buf tag_buf = {
                    .buf = iv->ptr,
                    .size = (const char *)copy.buf - (const char *)iv->ptr,
                };
                enum ppb_error tag_err = PPB_OK;
                uint64_t inner_num = ppb_decode_varint(&tag_buf, &tag_err) >> 3;
                if (tag_err != PPB_OK)
                {
                    fprintf(stderr, "picoscope: corrupt tag\n");
                    return 1;
                }

                printf("%" PRIu64 ": {", field_num);
                if (format_field(iidx, inner_num, iv, 0, /*newline=*/false) != 0)
                {
                    return 1;
                }

                printf("}%s", nl);
                return 0;
            }
        }

        printf("%" PRIu64 ": {\n", field_num);
        if (disassemble(payload, indent + 2) != 0)
        {
            return 1;
        }

        print_indent(indent);
        printf("}%s", nl);
        return 0;
    }

    /* 2. Try UTF-8 string (before packed varints: ASCII text is valid varints). */
    if (looks_like_string(payload))
    {
        printf("%" PRIu64 ": {\"", field_num);
        print_escaped_string(payload);
        printf("\"}%s", nl);
        return 0;
    }

    /* 3. Try packed varints (skipped in protoscope-compat mode). */
    if (!g_compat_mode && looks_like_packed_varints(payload))
    {
        printf("%" PRIu64 ": {", field_num);
        struct ppb_buf copy = payload;
        bool first = true;
        while (copy.size > 0)
        {
            enum ppb_error err = PPB_OK;
            uint64_t val = ppb_decode_varint(&copy, &err);
            if (err != PPB_OK)
            {
                break;
            }

            printf(first ? "%" PRIu64 : " %" PRIu64, val);
            first = false;
        }

        printf("}%s", nl);
        return 0;
    }

    /* 4. Hex fallback. */
    printf("%" PRIu64 ": {`", field_num);
    print_hex(payload);
    printf("`}%s", nl);
    return 0;
}

static int
format_field(size_t idx, uint64_t field_num, struct ppb_field_value *v, size_t indent, bool newline)
{
    const char *nl = newline ? "\n" : "";

    switch (idx)
    {
    case FIELD_VARINT:
        printf("%" PRIu64 ": %" PRId64 "%s", field_num, (int64_t)v->u64, nl);
        break;

    case FIELD_I64:
        printf("%" PRIu64 ": %" PRIu64 "i64%s", field_num, v->u64, nl);
        break;

    case FIELD_I32:
        printf("%" PRIu64 ": %" PRIu32 "i32%s", field_num, v->u32, nl);
        break;

    case FIELD_LEN:
        return format_len_field(field_num, v, indent, newline);
    }

    return 0;
}

static int
disassemble(struct ppb_buf buf, size_t indent)
{
    struct ppb_field fields[NUM_FIELDS] = {
        { .tag = PPB_TAG(-1, PPB_WIRE_VARINT) },
        { .tag = PPB_TAG(-1, PPB_WIRE_I64) },
        { .tag = PPB_TAG(-1, PPB_WIRE_LEN) },
        { .tag = PPB_TAG(-1, PPB_WIRE_I32) },
    };

    while (buf.size > 0)
    {
        struct ppb_lexn_ret ret = ppb_lexn(&buf, NUM_FIELDS, fields, 1);

        if (ret.status != PPB_OK)
        {
            fprintf(stderr, "picoscope: lex error %d\n", ret.status);
            return 1;
        }

        if (ret.field_range == 0)
        {
            break;
        }

        size_t idx = ret.first_field;
        struct ppb_field_value *v = &fields[idx].v;

        /* Recover field number by re-decoding the tag varint at v->ptr. */
        struct ppb_buf tag_buf = {
            .buf = v->ptr,
            .size = (const char *)buf.buf - (const char *)v->ptr,
        };
        enum ppb_error tag_err = PPB_OK;
        uint64_t raw_tag = ppb_decode_varint(&tag_buf, &tag_err);
        if (tag_err != PPB_OK)
        {
            fprintf(stderr, "picoscope: corrupt tag\n");
            return 1;
        }

        uint64_t field_num = raw_tag >> 3;

        print_indent(indent);

        if (format_field(idx, field_num, v, indent, /*newline=*/true) != 0)
        {
            return 1;
        }
    }

    return 0;
}

/*
 * Reads the entire contents of `f` into a malloc'd buffer.
 * Returns the buffer and sets *OUT_size.  Returns NULL on error.
 */
static void *
read_all(FILE *f, size_t *OUT_size)
{
    size_t cap = 4096;
    size_t len = 0;
    char *buf = malloc(cap);

    if (buf == NULL)
    {
        return NULL;
    }

    for (;;)
    {
        size_t n = fread(buf + len, 1, cap - len, f);
        len += n;
        if (n == 0)
        {
            if (feof(f))
            {
                break;
            }

            free(buf);
            return NULL;
        }

        if (len == cap)
        {
            cap *= 2;
            char *tmp = realloc(buf, cap);
            if (tmp == NULL)
            {
                free(buf);
                return NULL;
            }

            buf = tmp;
        }
    }

    *OUT_size = len;
    return buf;
}

int
main(int argc, char **argv)
{
    FILE *f = stdin;
    int i = 1;

    if (i < argc && strcmp(argv[i], "-p") == 0)
    {
        g_compat_mode = true;
        i++;
    }

    if (argc - i > 1)
    {
        fprintf(stderr, "Usage: picoscope [-p] [FILE]\n");
        return 1;
    }

    if (i < argc && strcmp(argv[i], "-") != 0)
    {
        f = fopen(argv[i], "rb");
        if (f == NULL)
        {
            fprintf(stderr, "picoscope: %s: %s\n", argv[i], strerror(errno));
            return 1;
        }
    }

    size_t size;
    void *data = read_all(f, &size);
    if (f != stdin)
    {
        fclose(f);
    }

    if (data == NULL)
    {
        fprintf(stderr, "picoscope: failed to read input\n");
        return 1;
    }

    struct ppb_buf buf = { .buf = data, .size = size };

    /* Validate top-level message. */
    ptrdiff_t scanned = ppb_prescan(buf, 0, NULL, SIZE_MAX);
    if (scanned < 0 || (size_t)scanned != size)
    {
        if (scanned < 0)
        {
            const char *msg;
            switch ((enum ppb_error)scanned)
            {
            case PPB_ERROR_TRUNCATED_DATA:
                msg = "truncated data";
                break;
            case PPB_ERROR_CORRUPT_VARINT:
                msg = "corrupt varint";
                break;
            case PPB_ERROR_CORRUPT_TAG:
                msg = "corrupt field tag";
                break;
            default:
                msg = "unknown error";
                break;
            }

            fprintf(stderr, "picoscope: %s\n", msg);
        }
        else
        {
            fprintf(stderr, "picoscope: trailing garbage at byte %td\n", scanned);
        }

        free(data);
        return 1;
    }

    int ret = disassemble(buf, 0);
    free(data);
    return ret;
}
