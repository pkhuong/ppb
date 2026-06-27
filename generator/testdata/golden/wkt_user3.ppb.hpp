#pragma once

#include <ppb/ppb.hpp>

// clang-format off
#include <ppb/wkt.ppb.hpp>

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
        // ppb::on_submessage<F::at, ::ppb_gen::google::protobuf::Timestamp::merge_schema>(...)
        ::ppb::message<F::at, ::ppb_gen::google::protobuf::Timestamp::merge_schema, ::ppb::field_semantics::singular>,
        // ppb::on_submessage<F::took, ::ppb_gen::google::protobuf::Duration::merge_schema>(...)
        ::ppb::message<F::took, ::ppb_gen::google::protobuf::Duration::merge_schema, ::ppb::field_semantics::singular>,
        // ppb::on_submessage<F::flag, ::ppb_gen::google::protobuf::BoolValue::merge_schema>(...)
        ::ppb::message<F::flag, ::ppb_gen::google::protobuf::BoolValue::merge_schema, ::ppb::field_semantics::singular>,
        // ppb::on_submessage<F::count, ::ppb_gen::google::protobuf::Int32Value::merge_schema>(...)
        ::ppb::message<F::count, ::ppb_gen::google::protobuf::Int32Value::merge_schema, ::ppb::field_semantics::singular>,
        // ppb::on_submessage<F::label, ::ppb_gen::google::protobuf::StringValue::merge_schema>(...)
        ::ppb::message<F::label, ::ppb_gen::google::protobuf::StringValue::merge_schema, ::ppb::field_semantics::singular>,
        // ppb::on_submessage<F::mask, ::ppb_gen::google::protobuf::FieldMask::merge_schema>(...)
        ::ppb::message<F::mask, ::ppb_gen::google::protobuf::FieldMask::merge_schema, ::ppb::field_semantics::singular>,
        // ppb::on_submessage<F::blob, ::ppb_gen::google::protobuf::Any::merge_schema>(...)
        ::ppb::message<F::blob, ::ppb_gen::google::protobuf::Any::merge_schema, ::ppb::field_semantics::singular>,
        // ppb::on_submessage<F::nothing, ::ppb_gen::google::protobuf::Empty::merge_schema>(...)
        ::ppb::message<F::nothing, ::ppb_gen::google::protobuf::Empty::merge_schema, ::ppb::field_semantics::singular>>;

    using merge_schema = schema;
}

