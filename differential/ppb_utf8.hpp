/*
 * UTF-8 validator for the libprotobuf reflection sink.  proto3
 * requires `string` fields to be well-formed UTF-8; the generated
 * setters call is_valid() to reject overlong encodings, UTF-16
 * surrogates (U+D800..U+DFFF), and code points above U+10FFFF.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ppb_gen::utf8
{

constexpr bool
is_valid(::std::string_view s) noexcept
{
    const ::std::size_t n = s.size();
    ::std::size_t i = 0;

    while (i < n)
    {
        const unsigned char b0 = static_cast<unsigned char>(s[i]);

        if (b0 < 0x80)
        {
            i++;
            continue;
        }

        ::std::size_t len;
        ::std::uint32_t cp;
        ::std::uint32_t lo;

        if ((b0 & 0xE0) == 0xC0)
        {
            len = 2;
            cp = b0 & 0x1F;
            lo = 0x80;
        }
        else if ((b0 & 0xF0) == 0xE0)
        {
            len = 3;
            cp = b0 & 0x0F;
            lo = 0x800;
        }
        else if ((b0 & 0xF8) == 0xF0)
        {
            len = 4;
            cp = b0 & 0x07;
            lo = 0x10000;
        }
        else
        {
            /* Stray continuation byte (0x80..0xBF) or illegal lead (0xF8..0xFF). */
            return false;
        }

        if (i + len > n)
            return false;

        for (::std::size_t k = 1; k < len; k++)
        {
            const unsigned char bk = static_cast<unsigned char>(s[i + k]);

            if ((bk & 0xC0) != 0x80)
                return false;

            cp = (cp << 6) | (bk & 0x3F);
        }

        if (cp < lo)
            return false;  /* overlong */

        if (cp > 0x10FFFF)
            return false;  /* out of range */

        if (cp >= 0xD800 && cp <= 0xDFFF)
            return false;  /* surrogate */

        i += len;
    }

    return true;
}

}  // namespace ppb_gen::utf8
