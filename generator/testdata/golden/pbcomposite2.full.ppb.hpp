#pragma once

#include <ppb/ppb.hpp>

// clang-format off

namespace ppb_gen::demo_comp2
{
    enum class Color : ::std::int32_t
    {
        UNSET = 0,
        RED = 1,
    };
}

namespace ppb_gen::demo_comp2::UEnums
{
    enum class F : ::std::int32_t
    {
        cs = 1,
    };

    constexpr ::std::size_t max_depth = 0;

    using schema = ::ppb::auto_schema<
        // ppb::on_each<F::cs>(...); closed proto2 enum: out-of-range values are dispatched, not diverted to unknown fields
        ::ppb::unpacked_enumerated<F::cs, ::ppb_gen::demo_comp2::Color>,
        ::ppb::packed_enumerated<F::cs, ::ppb_gen::demo_comp2::Color, ::ppb::field_semantics::always_lexn>,
        // ppb::on_unknown<>(...)
        ::ppb::detect_unknown_fields<>>;

    using merge_schema = schema;
}

namespace ppb_gen::demo_comp2::UFixeds
{
    enum class F : ::std::int32_t
    {
        f32s = 1,
        ds = 2,
    };

    constexpr ::std::size_t max_depth = 0;

    using schema = ::ppb::auto_schema<
        // ppb::on_each<F::f32s>(...)
        ::ppb::unpacked_fixed32<F::f32s>,
        ::ppb::packed_fixed32<F::f32s, ::ppb::field_semantics::always_lexn>,
        // ppb::on_each<F::ds>(...)
        ::ppb::unpacked_f64<F::ds>,
        ::ppb::packed_f64<F::ds, ::ppb::field_semantics::always_lexn>,
        // ppb::on_unknown<>(...)
        ::ppb::detect_unknown_fields<>>;

    using merge_schema = schema;
}

