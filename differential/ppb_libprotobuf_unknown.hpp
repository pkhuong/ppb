/*
 * Support helpers for the protoc-gen-ppb libprotobuf reflection sink:
 * retain unknown fields into a libprotobuf UnknownFieldSet.
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

/*
 * Decode the field number from the tag varint at f.v.ptr.
 *
 * f.v.ptr was set by ppb_prescan/ppb_lexn after a successful tag decode.
 * The tag varint at that position has therefore already been validated
 * by ppb.c, and re-decoding the same bytes with ppb_decode_varint cannot
 * fail: the bytes haven't changed.  prescan also guarantees the field
 * number is in [1, 2^29 - 1], so the `int` cast in the caller is safe.
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

template <ppb::wire_type W>
ppb_error
retain_unknown_field(const ppb_field &f, ::google::protobuf::Message *msg, std::span<const std::byte> input)
{
    int fn = static_cast<int>(unknown_field_number(f, input));
    auto *unknown = msg->GetReflection()->MutableUnknownFields(msg);

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
