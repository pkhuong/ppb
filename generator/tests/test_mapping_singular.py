from google.protobuf import descriptor_pb2 as d
import protoc_gen_ppb as gen

from tests.conftest import FakeSymbols, make_field as field

T = d.FieldDescriptorProto


def descr(f, syntax, strict_repeated_encoding=True, f_name="F", symbols=None):
    return gen.map_field(
        f,
        syntax=syntax,
        strict_repeated_encoding=strict_repeated_encoding,
        f_name=f_name,
        symbols=symbols or FakeSymbols(),
    )


def test_proto2_optional_scalar_is_last_write_wins():
    assert descr(field("x", 1, T.TYPE_INT32), "proto2") == ["::ppb::int32<F::x>"]
    assert descr(field("b", 4, T.TYPE_BOOL), "proto2") == ["::ppb::boolean<F::b>"]
    assert descr(field("d", 7, T.TYPE_DOUBLE), "proto2") == ["::ppb::f64<F::d>"]
    assert descr(field("ff", 8, T.TYPE_SFIXED32), "proto2") == ["::ppb::sfixed32<F::ff>"]


def test_proto3_implicit_presence_scalar_is_zero_default():
    assert descr(field("x", 1, T.TYPE_INT32), "proto3") == ["::ppb::proto3_int32<F::x>"]
    assert descr(field("u", 2, T.TYPE_UINT64), "proto3") == ["::ppb::proto3_uint64<F::u>"]
    assert descr(field("s", 3, T.TYPE_SINT32), "proto3") == ["::ppb::proto3_sint32<F::s>"]


def test_proto3_explicit_optional_scalar_has_presence():
    # proto3 `optional` => synthetic oneof => last_write_wins, NOT proto3_zero_default
    assert descr(field("x", 1, T.TYPE_INT32, proto3_optional=True), "proto3") == [
        "::ppb::int32<F::x>"
    ]


def test_enum_singular_maps_to_enumerated():
    f = field("c", 5, T.TYPE_ENUM, type_name=".pkg.Foo.Color")
    assert descr(f, "proto2") == ["::ppb::enumerated<F::c, ::pkg::Foo::Color>"]
    assert descr(f, "proto3") == ["::ppb::proto3_enumerated<F::c, ::pkg::Foo::Color>"]


def test_resolved_f_name_used_for_key():
    assert descr(field("x", 1, T.TYPE_INT32), "proto2", f_name="F_") == ["::ppb::int32<F_::x>"]


def test_proto2_required_scalar_is_last_write_wins():
    f = field("r", 1, T.TYPE_INT32, label=T.LABEL_REQUIRED)
    assert descr(f, "proto2") == ["::ppb::int32<F::r>"]


def test_singular_message_uses_merge_schema_and_singular_semantics():
    # A singular TYPE_MESSAGE field always uses merge_schema (repeated occurrences
    # merge into one child, so absent proto3 scalars must not clobber) and
    # field_semantics::singular (every occurrence dispatches, even if the
    # same as a prior one).
    f = field("m", 1, T.TYPE_MESSAGE, type_name=".pkg.Bar")
    result = gen.map_field(
        f, syntax="proto2", strict_repeated_encoding=True, f_name="F", symbols=FakeSymbols()
    )
    assert result == [
        "::ppb::message<F::m, ::pkg::Bar::merge_schema, ::ppb::field_semantics::singular>"
    ]
