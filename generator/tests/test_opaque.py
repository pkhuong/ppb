from google.protobuf import descriptor_pb2 as d
import protoc_gen_ppb as gen

from tests.conftest import FakeSymbols, make_field

T = d.FieldDescriptorProto


def msg_field(name, number, target, label=T.LABEL_OPTIONAL):
    return make_field(name, number, T.TYPE_MESSAGE, label=label, type_name=target)


def test_opaque_singular_message_becomes_bytes():
    f = msg_field("a", 1, ".pkg.A")
    got = gen.map_field(
        f,
        syntax="proto3",
        strict_repeated_encoding=True,
        f_name="F",
        symbols=FakeSymbols(),
        opaque=True,
    )
    assert got == [
        "::ppb::bytes<F::a, ::std::byte, ::ppb::field_semantics::singular> /* recursive: opaque */"
    ]


def test_opaque_repeated_message_becomes_unpacked_bytes():
    f = msg_field("a", 1, ".pkg.A", label=T.LABEL_REPEATED)
    got = gen.map_field(
        f,
        syntax="proto3",
        strict_repeated_encoding=True,
        f_name="F",
        symbols=FakeSymbols(),
        opaque=True,
    )
    assert got == ["::ppb::unpacked_bytes<F::a> /* recursive: opaque */"]


def test_non_opaque_default_still_typed():
    f = msg_field("a", 1, ".pkg.A")
    got = gen.map_field(
        f, syntax="proto3", strict_repeated_encoding=True, f_name="F", symbols=FakeSymbols()
    )
    assert got == ["::ppb::message<F::a, ::pkg::A::merge_schema, ::ppb::field_semantics::singular>"]


def test_opaque_singular_message_uses_singular_semantics():
    # A singular opaque-recursive field always gets
    # ::ppb::field_semantics::singular so every wire occurrence is dispatched
    # and a handler can merge them field-by-field.
    f = msg_field("a", 1, ".pkg.A")
    got = gen.map_field(
        f,
        syntax="proto3",
        strict_repeated_encoding=True,
        f_name="F",
        symbols=FakeSymbols(),
        opaque=True,
    )
    assert got == [
        "::ppb::bytes<F::a, ::std::byte, ::ppb::field_semantics::singular> /* recursive: opaque */"
    ]


def test_opaque_repeated_message_stays_unpacked():
    f = msg_field("a", 1, ".pkg.A", label=T.LABEL_REPEATED)
    got = gen.map_field(
        f,
        syntax="proto3",
        strict_repeated_encoding=True,
        f_name="F",
        symbols=FakeSymbols(),
        opaque=True,
    )
    assert got == ["::ppb::unpacked_bytes<F::a> /* recursive: opaque */"]
