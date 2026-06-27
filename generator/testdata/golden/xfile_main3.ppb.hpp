#pragma once

#include <ppb/ppb.hpp>

// clang-format off
#include "xfile_base3.ppb.hpp"

namespace ppb_gen::xfile_main::Main
{
    enum class F : ::std::int32_t
    {
        leaf = 1,
        shared = 2,
        kind = 3,
    };

    constexpr ::std::size_t max_depth = 1;

    using schema = ::ppb::auto_schema<
        // ppb::on_submessage<F::leaf, ::ppb_gen::xfile_base::Leaf::merge_schema>(...)
        ::ppb::message<F::leaf, ::ppb_gen::xfile_base::Leaf::merge_schema, ::ppb::field_semantics::singular>,
        // ppb::on<F::shared>(...)
        ::ppb::proto3_enumerated<F::shared, ::ppb_gen::xfile_base::Shared>,
        // ppb::on<F::kind>(...)
        ::ppb::proto3_enumerated<F::kind, ::ppb_gen::xfile_base::Leaf::Kind>>;

    using merge_schema = ::ppb::auto_schema<
        // ppb::on_submessage<F::leaf, ::ppb_gen::xfile_base::Leaf::merge_schema>(...)
        ::ppb::message<F::leaf, ::ppb_gen::xfile_base::Leaf::merge_schema, ::ppb::field_semantics::singular>,
        // ppb::on<F::shared>(...)
        ::ppb::enumerated<F::shared, ::ppb_gen::xfile_base::Shared>,
        // ppb::on<F::kind>(...)
        ::ppb::enumerated<F::kind, ::ppb_gen::xfile_base::Leaf::Kind>>;
}

