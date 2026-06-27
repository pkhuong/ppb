#pragma once

#include <ppb/ppb.hpp>

// clang-format off

namespace ppb_gen::demo_recp::Node
{
    enum class F : ::std::int32_t
    {
        value = 1,
        next = 2,
    };

    constexpr ::std::size_t max_depth = 0;

    using schema = ::ppb::auto_schema<
        // ppb::on<F::value>(...)
        ::ppb::proto3_int32<F::value>,
        // ppb::on_submessage<F::next, ::ppb_gen::demo_recp::Node::merge_schema>(...)
        ::ppb::bytes<F::next, ::std::byte, ::ppb::field_semantics::singular> /* recursive: opaque */,
        // ppb::on_unknown<>(...)
        ::ppb::detect_unknown_fields<>>;

    using merge_schema = ::ppb::auto_schema<
        // ppb::on<F::value>(...)
        ::ppb::int32<F::value>,
        // ppb::on_submessage<F::next, ::ppb_gen::demo_recp::Node::merge_schema>(...)
        ::ppb::bytes<F::next, ::std::byte, ::ppb::field_semantics::singular> /* recursive: opaque */,
        // ppb::on_unknown<>(...)
        ::ppb::detect_unknown_fields<>>;
}

