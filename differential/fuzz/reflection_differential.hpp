#pragma once

/*
 * Parameterized differential checker.
 */

#include "ppb/ppb.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <google/protobuf/message.h>
#include <google/protobuf/unknown_field_set.h>
#include <google/protobuf/util/field_comparator.h>
#include <google/protobuf/util/message_differencer.h>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#ifndef POSTCOND
#define POSTCOND(cond)        \
    do                        \
    {                         \
        if (!(cond))          \
            __builtin_trap(); \
    } while (0)
#endif

namespace ppb_fuzz
{

/*
 * Controls which documented leniency categories allow early return
 * (skip) instead of trapping.
 */
struct LeniencyPolicy
{
    bool tolerate_overlong_framing = true;
    bool tolerate_reparse_survival = true;
};

using ::google::protobuf::Message;
using ::google::protobuf::UnknownFieldSet;

inline void
swallow_log(google::protobuf::LogLevel, const char *, int, const std::string &)
{
    // Keep logs clean of complaints about malformed utf-8, etc.
}

inline const bool g_log_handler_installed = (google::protobuf::SetLogHandler(swallow_log), true);

inline bool
unknown_set_contains_group(const UnknownFieldSet &set)
{
    for (int i = 0; i < set.field_count(); i++)
    {
        const auto &field = set.field(i);

        if (field.type() == google::protobuf::UnknownField::TYPE_GROUP)
            return true;
    }

    return false;
}

/* Did libprotobuf parse a group anywhere in `msg`? */
inline bool
message_contains_group(const Message &msg)
{
    const auto *refl = msg.GetReflection();

    if (unknown_set_contains_group(refl->GetUnknownFields(msg)))
        return true;

    std::vector<const google::protobuf::FieldDescriptor *> fields;
    refl->ListFields(msg, &fields);

    for (const auto *fd : fields)
    {
        if (fd->cpp_type() != google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE)
            continue;

        if (fd->is_repeated())
        {
            const int n = refl->FieldSize(msg, fd);

            for (int i = 0; i < n; i++)
            {
                if (message_contains_group(refl->GetRepeatedMessage(msg, fd, i)))
                    return true;
            }
        }
        else if (message_contains_group(refl->GetMessage(msg, fd)))
        {
            return true;
        }
    }

    return false;
}

inline std::span<const std::byte>
as_bytes(const std::string &s)
{
    return std::span<const std::byte>(reinterpret_cast<const std::byte *>(s.data()), s.size());
}

/*
 * Pop one varint off the front of `*wire`; returns its value and
 * encoded length, or nullopt when the front is truncated or longer
 * than 10 bytes.
 */
inline std::optional<std::pair<uint64_t, size_t>>
pop_varint(std::span<const std::byte> *wire)
{
    uint64_t value = 0;
    size_t len = 0;

    while (len < wire->size() && len < 10)
    {
        const auto b = static_cast<uint8_t>((*wire)[len]);

        value |= static_cast<uint64_t>(b & 0x7f) << (7 * len);
        len++;

        if (b < 0x80)
        {
            *wire = wire->subspan(len);
            return std::pair { value, len };
        }
    }

    return std::nullopt;
}

/*
 * Does `wire` contain a *framing* varint (a tag, or a LEN length)
 * of 6+ encoded bytes?  libprotobuf rejects those outright after 5 bytes,
 * while PPB decodes them correctly and then validates.
 */
inline bool
wire_has_overlong_framing_varint(std::span<const std::byte> wire)
{
    bool found = false;

    while (!wire.empty())
    {
        const auto tag = pop_varint(&wire);
        if (!tag)
            return found;

        found |= tag->second >= 6;

        switch (tag->first & 7)
        {
        case 0:
            if (!pop_varint(&wire))
                return found;

            break;

        case 1:
        case 5:
        {
            const size_t width = (tag->first & 7) == 1 ? 8 : 4;
            if (wire.size() < width)
                return found;

            wire = wire.subspan(width);
            break;
        }

        case 2:
        {
            const auto paylen = pop_varint(&wire);
            if (!paylen || paylen->first > wire.size())
                return found;

            found |= paylen->second >= 6;
            found |= wire_has_overlong_framing_varint(wire.first(static_cast<size_t>(paylen->first)));
            wire = wire.subspan(static_cast<size_t>(paylen->first));
            break;
        }

        default:
            return found;
        }
    }

    return found;
}

template <typename Msg>
bool
messages_equal(const Msg &a, const Msg &b)
{
    google::protobuf::util::MessageDifferencer diff;
    google::protobuf::util::DefaultFieldComparator cmp;

    cmp.set_treat_nan_as_equal(true);
    diff.set_field_comparator(&cmp);
    return diff.Compare(a, b);
}

/*
 * Does PPB decode the reference's *canonical* re-serialization back
 * to the reference?  When it does, any disagreement on the original
 * bytes is due to different handling of *non-canonical* wire encoding.
 *
 * An actual canonical value misdecode would also corrupt the
 * canonical form, so this relaxed check never papers over that important
 * bug class.
 */
template <typename Msg, typename ParseInto>
bool
ppb_agrees_on_canonical(const Msg &reference, ParseInto parse_into)
{
    Msg redecoded;
    return parse_into(as_bytes(reference.SerializePartialAsString()), &redecoded) == PPB_OK &&
        messages_equal(reference, redecoded);
}

template <LeniencyPolicy P, typename Msg, typename ParseInto>
void
differential_check(const char *label, std::span<const std::byte> input, ParseInto parse_into)
{
    const std::string wire(reinterpret_cast<const char *>(input.data()), input.size());

    Msg reference;
    const bool reference_ok = reference.ParsePartialFromString(wire);

    Msg decoded;
    const ppb_error err = parse_into(input, &decoded);

    if (reference_ok && err == PPB_OK)
    {
        if (!messages_equal(reference, decoded) && !ppb_agrees_on_canonical(reference, parse_into))
        {
            /*
             * The canonical-agreement check is useless when the
             * reference contains a group in an unknown-field set:
             * libprotobuf re-serializes the group verbatim while PPB
             * rejects wire types 3/4.
             */
            if (message_contains_group(reference))
                return;

            fprintf(stderr, "%s: value divergence on %zu accepted bytes\nreference:\n%s\ndecoded:\n%s\n",
                label, wire.size(), reference.DebugString().c_str(), decoded.DebugString().c_str());
            POSTCOND(false);
        }

        return;
    }

    if (reference_ok)
    {
        if (message_contains_group(reference))
            return;

        /*
         * PPB rejected but the reference accepted.  This can happen
         * for non-canonical inputs, like when libprotobuf truncates
         * tag varints to 32-bits before checking them.
         *
         * Assume something like this is happening when the two
         * implementations agree on the canonical encoding.
         */
        if (ppb_agrees_on_canonical(reference, parse_into))
            return;

        fprintf(stderr,
            "%s: ppb rejected (ppb_error %d) %zu bytes libprotobuf accepts (no group), and rejects "
            "or mis-decodes the canonical re-serialization\nreference:\n%s\n",
            label, static_cast<int>(err), wire.size(), reference.DebugString().c_str());
        POSTCOND(false);
    }

    if (err == PPB_OK)
    {
        if constexpr (P.tolerate_overlong_framing)
        {
            if (wire_has_overlong_framing_varint(input))
                return;
        }

        /*
         * libprotobuf rejected the bytes, PPB accepted.  Acceptable
         * when PPB did not *launder* bad bytes into something that
         * libprotobuf accepts.
         */
        if constexpr (P.tolerate_reparse_survival)
        {
            Msg reparsed;
            if (!reparsed.ParsePartialFromString(decoded.SerializePartialAsString()))
                return;
        }

        fprintf(stderr,
            "%s: ppb decoded %zu libprotobuf-rejected bytes into content libprotobuf accepts\n"
            "decoded:\n%s\n",
            label, wire.size(), decoded.DebugString().c_str());
        POSTCOND(false);
    }
}

}  // namespace ppb_fuzz
