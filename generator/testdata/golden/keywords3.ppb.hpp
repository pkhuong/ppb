#pragma once

#include <ppb/ppb.hpp>

// clang-format off

namespace ppb_gen::demo_kw
{
    enum class Choice : ::std::int32_t
    {
        default_ = 0,
        OK = 1,
    };
}

namespace ppb_gen::demo_kw::Class
{
    enum class F : ::std::int32_t
    {
        int_ = 1,
        namespace_ = 2,
        case_ = 3,
    };

    constexpr ::std::size_t max_depth = 0;

    using schema = ::ppb::auto_schema<
        ::ppb::proto3_int32<F::int_>,
        ::ppb::proto3_utf8string<F::namespace_>,
        ::ppb::proto3_enumerated<F::case_, ::ppb_gen::demo_kw::Choice>>;

    using merge_schema = ::ppb::auto_schema<
        ::ppb::int32<F::int_>,
        ::ppb::utf8string<F::namespace_>,
        ::ppb::enumerated<F::case_, ::ppb_gen::demo_kw::Choice>>;
}

