/*
 * Support helpers for the protoc-gen-ppb libprotobuf reflection sink:
 * retain unknown fields into a libprotobuf UnknownFieldSet, and reject
 * out-of-range field numbers.
 */
#pragma once

#include "ppb/ppb.h"
#include "ppb/ppb.hpp"

#include <cstdint>
#include <google/protobuf/message.h>
#include <google/protobuf/unknown_field_set.h>
#include <string>

namespace ppb_sink
{

/* Maximum valid protobuf field number: 2^29 - 1. */
inline constexpr uint64_t k_max_field_number = (1U << 29) - 1;

/*
 * Decode the field number from the tag varint at f.v.ptr.
 *
 * f.v.ptr was set by ppb_prescan/ppb_lexn after a successful tag decode.
 * The tag varint at that position has therefore already been validated
 * by ppb.c, and re-decoding the same bytes with ppb_decode_varint cannot
 * fail: the bytes haven't changed.
 */
inline uint64_t
unknown_field_number(const ppb_field &f, std::span<const std::byte> input)
{
    const auto *ptr = static_cast<const std::byte *>(f.v.ptr);
    const std::byte *end = input.data() + input.size();
    size_t available = static_cast<size_t>(end - ptr);

    struct ppb_buf buf = { ptr, available };
    enum ppb_error err = PPB_OK;
    uint64_t tag = ppb_decode_varint(&buf, &err);

    return tag >> 3;
}

/*
 * Field-number validation without retention, for map-entry scopes:
 * libprotobuf drops unknown fields when it syncs entries into the
 * map, but invalid field numbers must still be rejected.
 */
inline ppb_error
validate_unknown_field(const ppb_field &f, std::span<const std::byte> input)
{
    uint64_t field_number = unknown_field_number(f, input);

    /*
     * field-number-range: out-of-range field numbers are rejected.  Field 0
     * (canonical or overlong) never reaches an unknown handler: ppb rejects
     * it during the prescan pass that precedes dispatch.
     */
    if (field_number > k_max_field_number)
    {
        return PPB_ERROR_CORRUPT_TAG;
    }

    return PPB_OK;
}

template <ppb::wire_type W>
ppb_error
retain_unknown_field(const ppb_field &f, ::google::protobuf::Message *msg, std::span<const std::byte> input)
{
    uint64_t field_number = unknown_field_number(f, input);

    /*
     * field-number-range: out-of-range field numbers are rejected.  Field 0
     * (canonical or overlong) never reaches an unknown handler: ppb rejects
     * it during the prescan pass that precedes dispatch.
     */
    if (field_number > k_max_field_number)
    {
        return PPB_ERROR_CORRUPT_TAG;
    }

    auto *unknown = msg->GetReflection()->MutableUnknownFields(msg);
    int fn = static_cast<int>(field_number);

    if constexpr (W == ppb::wire_type::varint)
    {
        unknown->AddVarint(fn, f.v.u64);
    }
    else if constexpr (W == ppb::wire_type::i64)
    {
        unknown->AddFixed64(fn, f.v.u64);
    }
    else if constexpr (W == ppb::wire_type::i32)
    {
        unknown->AddFixed32(fn, static_cast<uint32_t>(f.v.u64));
    }
    else if constexpr (W == ppb::wire_type::len)
    {
        const auto *p = static_cast<const char *>(f.v.payload.buf);
        unknown->AddLengthDelimited(fn, std::string(p, f.v.payload.size));
    }
    else
    {
        static_assert(W == ppb::wire_type::varint, "unsupported wire type for retention");
    }

    return PPB_OK;
}

}  // namespace ppb_sink
