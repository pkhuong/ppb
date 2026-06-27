#pragma once

#include <ppb/ppb.hpp>

// clang-format off

namespace ppb_gen::xfile_base
{
    enum class Shared : ::std::int32_t
    {
        S_UNSET = 0,
        S_ON = 1,
    };
}

namespace ppb_gen::xfile_base::Leaf
{
    enum class Kind : ::std::int32_t
    {
        K_NONE = 0,
        K_A = 1,
    };
}

namespace ppb_gen::xfile_base::Leaf
{
    enum class F : ::std::int32_t
    {
        id = 1,
        kind = 2,
    };

    constexpr ::std::size_t max_depth = 0;

    using schema = ::ppb::auto_schema<
        // ppb::on<F::id>(...)
        ::ppb::proto3_int32<F::id>,
        // ppb::on<F::kind>(...)
        ::ppb::proto3_enumerated<F::kind, ::ppb_gen::xfile_base::Leaf::Kind>>;

    using merge_schema = ::ppb::auto_schema<
        // ppb::on<F::id>(...)
        ::ppb::int32<F::id>,
        // ppb::on<F::kind>(...)
        ::ppb::enumerated<F::kind, ::ppb_gen::xfile_base::Leaf::Kind>>;
}

