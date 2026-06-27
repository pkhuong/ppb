#pragma once

#include <ppb/ppb.hpp>

// clang-format off
#include <ppb/wkt.ppb.hpp>

namespace ppb_gen::google::protobuf::Timestamp
{
    using schema_with_unknowns = ::ppb::auto_schema<schema, ::ppb::detect_unknown_fields<>>;
    using merge_schema_with_unknowns = ::ppb::auto_schema<merge_schema, ::ppb::detect_unknown_fields<>>;
}

namespace ppb_gen::ppb::Event
{
    enum class F : ::std::int32_t
    {
        at = 1,
        seq = 2,
    };

    constexpr ::std::size_t max_depth = 1;

    using schema = ::ppb::auto_schema<
        // ppb::on_submessage<F::at, ::ppb_gen::google::protobuf::Timestamp::merge_schema_with_unknowns>(...)
        ::ppb::message<F::at, ::ppb_gen::google::protobuf::Timestamp::merge_schema_with_unknowns, ::ppb::field_semantics::singular>,
        // ppb::on<F::seq>(...)
        ::ppb::proto3_int32<F::seq>,
        // ppb::on_unknown<>(...)
        ::ppb::detect_unknown_fields<>>;

    using merge_schema = ::ppb::auto_schema<
        // ppb::on_submessage<F::at, ::ppb_gen::google::protobuf::Timestamp::merge_schema_with_unknowns>(...)
        ::ppb::message<F::at, ::ppb_gen::google::protobuf::Timestamp::merge_schema_with_unknowns, ::ppb::field_semantics::singular>,
        // ppb::on<F::seq>(...)
        ::ppb::int32<F::seq>,
        // ppb::on_unknown<>(...)
        ::ppb::detect_unknown_fields<>>;
}

