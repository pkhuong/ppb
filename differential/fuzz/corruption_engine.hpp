/*
 * Protobuf-free byte-corruption engine for the structured+byte
 * corruption fuzzer.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ppb_fuzz
{

inline constexpr std::size_t k_max_ops = 8;

struct CorruptOpPlain
{
    enum Kind
    {
        CHANGE = 0,
        INSERT = 1,
        DELETE = 2,
    };

    Kind kind;
    std::uint32_t position;
    std::uint8_t value;
};

enum class CorruptionVariant
{
    ANY,
    NO_OVERLONG,
    ONE_TAG,
};

/*
 * True iff a CHANGE of byte at `pos` to `value` will definitely not introduce
 * overlong / over-5-byte framing varint:
 *   - preserve bit 7 (continuation structure unchanged), and
 *   - do not zero the most-significant byte of a multi-byte varint run
 *     (forbid value 0x00 when the previous byte has bit 7 set).
 */
inline bool
no_overlong_change_ok(std::string_view buf, std::size_t pos, std::uint8_t value)
{
    const std::uint8_t old = static_cast<std::uint8_t>(buf[pos]);

    if ((old & 0x80) != (value & 0x80))
        return false;

    if (value == 0x00 && pos > 0 && (static_cast<std::uint8_t>(buf[pos - 1]) & 0x80) != 0)
        return false;

    return true;
}

/*
 * Offsets of toplevel tag varints in `wire` (generic protobuf walk;
 * LEN payloads skipped opaquely; stops cleanly on truncation).
 */
inline std::vector<std::size_t>
tag_positions(std::string_view wire)
{
    std::vector<std::size_t> tags;
    std::size_t i = 0;

    while (i < wire.size())
    {
        tags.push_back(i);

        /* decode the tag varint */
        std::uint64_t tag = 0;
        int shift = 0;
        std::size_t j = i;
        bool ok = false;
        for (; j < wire.size() && shift < 64; shift += 7)
        {
            const std::uint8_t b = static_cast<std::uint8_t>(wire[j++]);

            tag |= static_cast<std::uint64_t>(b & 0x7f) << shift;
            if ((b & 0x80) == 0)
            {
                ok = true;
                break;
            }
        }

        if (!ok)
            break;

        const std::uint32_t wire_type = static_cast<std::uint32_t>(tag & 0x7);

        if (wire_type == 0)  /* varint value */
        {
            while (j < wire.size() && (static_cast<std::uint8_t>(wire[j]) & 0x80))
                j++;

            if (j >= wire.size())
                break;

            j++;
        }
        else if (wire_type == 1)  /* i64 */
        {
            j += 8;
        }
        else if (wire_type == 5)  /* i32 */
        {
            j += 4;
        }
        else if (wire_type == 2)  /* len */
        {
            std::uint64_t len = 0;
            int s2 = 0;
            bool lok = false;
            for (; j < wire.size() && s2 < 64; s2 += 7)
            {
                const std::uint8_t b = static_cast<std::uint8_t>(wire[j++]);

                len |= static_cast<std::uint64_t>(b & 0x7f) << s2;
                if ((b & 0x80) == 0)
                {
                    lok = true;
                    break;
                }
            }

            if (!lok)
                break;

            j += len;
        }
        else  /* groups / unknown wire type: stop */
        {
            break;
        }

        if (j > wire.size())
            break;

        i = j;
    }

    return tags;
}

/* Applies up to k_max_ops byte-ops in sequence to a copy of `wire`. */
inline std::string
apply_corruptions(std::string_view wire, std::span<const CorruptOpPlain> ops, CorruptionVariant variant)
{
    std::string out(wire);
    std::size_t applied = 0;

    for (const CorruptOpPlain &op : ops)
    {
        if (applied >= k_max_ops)
            break;

        applied++;

        const std::size_t len = out.size();

        if (variant == CorruptionVariant::NO_OVERLONG)
        {
            if (op.kind != CorruptOpPlain::CHANGE)
                continue;

            if (len == 0)
                continue;

            const std::size_t pos = op.position % len;

            if (!no_overlong_change_ok(out, pos, op.value))
                continue;

            out[pos] = static_cast<char>(op.value);
            continue;
        }

        if (variant == CorruptionVariant::ONE_TAG)
        {
            if (op.kind != CorruptOpPlain::CHANGE)
                continue;

            const std::vector<std::size_t> tags = tag_positions(out);

            if (tags.empty())
                continue;

            const std::size_t pos = tags[op.position % tags.size()];

            out[pos] = static_cast<char>(op.value);
            break;  /* at most one tag change */
        }

        if (op.kind == CorruptOpPlain::CHANGE)
        {
            if (len == 0)
                continue;

            out[op.position % len] = static_cast<char>(op.value);
        }
        else if (op.kind == CorruptOpPlain::INSERT)
        {
            out.insert(out.begin() + static_cast<std::string::difference_type>(op.position % (len + 1)),
                static_cast<char>(op.value));
        }
        else  /* DELETE */
        {
            if (len == 0)
                continue;

            out.erase(out.begin() + static_cast<std::string::difference_type>(op.position % len));
        }
    }

    return out;
}

}  // namespace ppb_fuzz
