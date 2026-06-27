#pragma once

#include <ppb/ppb.hpp>

// clang-format off
#include <ppb/wkt.ppb.hpp>

namespace ppb_gen::google::protobuf::Timestamp
{
    using schema_with_unknowns = ::ppb::auto_schema<schema, ::ppb::detect_unknown_fields<>>;
    using merge_schema_with_unknowns = ::ppb::auto_schema<merge_schema, ::ppb::detect_unknown_fields<>>;
}

namespace ppb_gen::google::protobuf::Duration
{
    using schema_with_unknowns = ::ppb::auto_schema<schema, ::ppb::detect_unknown_fields<>>;
    using merge_schema_with_unknowns = ::ppb::auto_schema<merge_schema, ::ppb::detect_unknown_fields<>>;
}

namespace ppb_gen::google::protobuf::BoolValue
{
    using schema_with_unknowns = ::ppb::auto_schema<schema, ::ppb::detect_unknown_fields<>>;
    using merge_schema_with_unknowns = ::ppb::auto_schema<merge_schema, ::ppb::detect_unknown_fields<>>;
}

namespace ppb_gen::google::protobuf::Int32Value
{
    using schema_with_unknowns = ::ppb::auto_schema<schema, ::ppb::detect_unknown_fields<>>;
    using merge_schema_with_unknowns = ::ppb::auto_schema<merge_schema, ::ppb::detect_unknown_fields<>>;
}

namespace ppb_gen::google::protobuf::StringValue
{
    using schema_with_unknowns = ::ppb::auto_schema<schema, ::ppb::detect_unknown_fields<>>;
    using merge_schema_with_unknowns = ::ppb::auto_schema<merge_schema, ::ppb::detect_unknown_fields<>>;
}

namespace ppb_gen::google::protobuf::FieldMask
{
    using schema_with_unknowns = ::ppb::auto_schema<schema, ::ppb::detect_unknown_fields<>>;
    using merge_schema_with_unknowns = ::ppb::auto_schema<merge_schema, ::ppb::detect_unknown_fields<>>;
}

namespace ppb_gen::google::protobuf::Any
{
    using schema_with_unknowns = ::ppb::auto_schema<schema, ::ppb::detect_unknown_fields<>>;
    using merge_schema_with_unknowns = ::ppb::auto_schema<merge_schema, ::ppb::detect_unknown_fields<>>;
}

namespace ppb_gen::wktdemo::All
{
    enum class F : ::std::int32_t
    {
        at = 1,
        took = 2,
        flag = 3,
        count = 4,
        label = 5,
        mask = 6,
        blob = 7,
        nothing = 8,
    };

    constexpr ::std::size_t max_depth = 1;

    using schema = ::ppb::auto_schema<
        // ppb::on_submessage<F::at, ::ppb_gen::google::protobuf::Timestamp::merge_schema_with_unknowns>(...)
        ::ppb::message<F::at, ::ppb_gen::google::protobuf::Timestamp::merge_schema_with_unknowns, ::ppb::field_semantics::singular>,
        // ppb::on_submessage<F::took, ::ppb_gen::google::protobuf::Duration::merge_schema_with_unknowns>(...)
        ::ppb::message<F::took, ::ppb_gen::google::protobuf::Duration::merge_schema_with_unknowns, ::ppb::field_semantics::singular>,
        // ppb::on_submessage<F::flag, ::ppb_gen::google::protobuf::BoolValue::merge_schema_with_unknowns>(...)
        ::ppb::message<F::flag, ::ppb_gen::google::protobuf::BoolValue::merge_schema_with_unknowns, ::ppb::field_semantics::singular>,
        // ppb::on_submessage<F::count, ::ppb_gen::google::protobuf::Int32Value::merge_schema_with_unknowns>(...)
        ::ppb::message<F::count, ::ppb_gen::google::protobuf::Int32Value::merge_schema_with_unknowns, ::ppb::field_semantics::singular>,
        // ppb::on_submessage<F::label, ::ppb_gen::google::protobuf::StringValue::merge_schema_with_unknowns>(...)
        ::ppb::message<F::label, ::ppb_gen::google::protobuf::StringValue::merge_schema_with_unknowns, ::ppb::field_semantics::singular>,
        // ppb::on_submessage<F::mask, ::ppb_gen::google::protobuf::FieldMask::merge_schema_with_unknowns>(...)
        ::ppb::message<F::mask, ::ppb_gen::google::protobuf::FieldMask::merge_schema_with_unknowns, ::ppb::field_semantics::singular>,
        // ppb::on_submessage<F::blob, ::ppb_gen::google::protobuf::Any::merge_schema_with_unknowns>(...)
        ::ppb::message<F::blob, ::ppb_gen::google::protobuf::Any::merge_schema_with_unknowns, ::ppb::field_semantics::singular>,
        // ppb::on_submessage<F::nothing, ::ppb_gen::google::protobuf::Empty::merge_schema>(...)
        ::ppb::message<F::nothing, ::ppb_gen::google::protobuf::Empty::merge_schema, ::ppb::field_semantics::singular>,
        // ppb::on_unknown<>(...)
        ::ppb::detect_unknown_fields<>>;

    using merge_schema = schema;
}

