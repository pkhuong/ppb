#include "ppb_utf8.hpp"

#include <cstdio>
#include <string_view>

namespace
{

using ppb_gen::utf8::is_valid;

constexpr bool
valid(const char *s, std::size_t n)
{
    return is_valid(std::string_view(s, n));
}

/* Empty and pure ASCII. */
static_assert(valid("", 0));
static_assert(valid("hello", 5));
static_assert(valid("\x00\x7f", 2));  /* NUL and DEL are valid ASCII */

/* Well-formed multibyte (boundaries of each length class). */
static_assert(valid("\xc2\x80", 2));          /* U+0080  (2-byte min) */
static_assert(valid("\xdf\xbf", 2));          /* U+07FF  (2-byte max) */
static_assert(valid("\xe0\xa0\x80", 3));      /* U+0800  (3-byte min) */
static_assert(valid("\xef\xbf\xbf", 3));      /* U+FFFF  (3-byte max) */
static_assert(valid("\xf0\x90\x80\x80", 4));  /* U+10000 (4-byte min) */
static_assert(valid("\xf4\x8f\xbf\xbf", 4));  /* U+10FFFF (4-byte max) */

/* Stray / truncated continuation. */
static_assert(!valid("\x80", 1));          /* lone continuation byte */
static_assert(!valid("\xc2", 1));          /* truncated 2-byte lead */
static_assert(!valid("\xe0\xa0", 2));      /* truncated 3-byte lead */
static_assert(!valid("\xf0\x90\x80", 3));  /* truncated 4-byte lead */
static_assert(!valid("\xc2\x20", 2));      /* 2-byte lead, non-continuation */

/* Overlong encodings. */
static_assert(!valid("\xc0\x80", 2));              /* overlong U+0000 */
static_assert(!valid("\xc1\xbf", 2));              /* overlong U+007F */
static_assert(!valid("\xe0\x80\x80", 3));          /* overlong (3-byte for <0x800) */
static_assert(!valid("\xe0\x9f\xbf", 3));          /* overlong U+07FF */
static_assert(!valid("\xf0\x80\x80\x80", 4));      /* overlong (4-byte for <0x10000) */
static_assert(!valid("\xf0\x8f\xbf\xbf", 4));      /* overlong U+FFFF */

/* Surrogates U+D800..U+DFFF (always rejected). */
static_assert(!valid("\xed\xa0\x80", 3));  /* U+D800 */
static_assert(!valid("\xed\xbf\xbf", 3));  /* U+DFFF */

/* Above U+10FFFF / illegal lead bytes. */
static_assert(!valid("\xf4\x90\x80\x80", 4));  /* U+110000 */
static_assert(!valid("\xf5\x80\x80\x80", 4));  /* >U+10FFFF lead */
static_assert(!valid("\xff", 1));              /* 0xFF never a lead */
static_assert(!valid("\xfe", 1));              /* 0xFE never a lead */

/* Invalid byte mid-string must reject the whole string. */
static_assert(!valid("ok\xc0zz", 5));

struct vec
{
    const char *s;
    std::size_t n;
    bool ok;
};

constexpr vec kVecs[] = {
    { "", 0, true },
    { "hello", 5, true },
    { "\xc2\x80", 2, true },
    { "\xf4\x8f\xbf\xbf", 4, true },
    { "\x80", 1, false },
    { "\xc0\x80", 2, false },
    { "\xed\xa0\x80", 3, false },
    { "\xf4\x90\x80\x80", 4, false },
};

}  // namespace

int
main()
{
    int failures = 0;

    for (const vec &v : kVecs)
    {
        if (is_valid(std::string_view(v.s, v.n)) != v.ok)
        {
            fprintf(stderr, "utf8 mismatch: len=%zu expected=%d\n", v.n, v.ok);
            failures++;
        }
    }

    if (failures != 0)
        return 1;

    return 0;
}
