#pragma once

#include <ppb/ppb.hpp>

// clang-format off

namespace ppb_gen::defwarn::Foo
{
    enum class F : ::std::int32_t
    {
        x = 1,
        y = 2,
        s = 3,
        b = 4,
        z = 5,
        r = 6,
    };

    constexpr ::std::size_t max_depth = 0;

    using schema = ::ppb::auto_schema<
        // ppb::on<F::x>(...)
        ::ppb::int32<F::x>,
        // ppb::on<F::y>(...); default = '42'
        ::ppb::int32<F::y>,
        // ppb::on<F::s>(...); default = 'hello'
        ::ppb::utf8string<F::s>,
        // ppb::on<F::b>(...); default = 'true'
        ::ppb::boolean<F::b>,
        // ppb::on<F::z>(...)
        ::ppb::int32<F::z>,
        // ppb::on<F::r>(...); default = '99'
        ::ppb::int32<F::r>>;

    using merge_schema = schema;
}

