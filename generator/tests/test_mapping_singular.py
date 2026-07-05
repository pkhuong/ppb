from google.protobuf import descriptor_pb2 as d
import protoc_gen_ppb as gen

from tests.conftest import FakeSymbols, make_field as field

T = d.FieldDescriptorProto


def descr(
    f,
    syntax,
    strict_repeated_encoding=True,
    f_name="F",
    symbols=None,
    always_dispatch_strings=False,
):
    return gen.map_field(
        f,
        syntax=syntax,
        strict_repeated_encoding=strict_repeated_encoding,
        f_name=f_name,
        symbols=symbols or FakeSymbols(),
        always_dispatch_strings=always_dispatch_strings,
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


def test_proto3_real_oneof_member_is_always_lexn():
    # A real oneof member (oneof_index set, proto3_optional=False) must NOT get
    # the zero-default treatment even in proto3; oneof fields always have
    # explicit presence.  It is emitted with always_lexn semantics so that any
    # present member forces a wire-order lexn pass; under oneof_as_optional that
    # makes last-occurrence semantics correct even when wire order disagrees with
    # field-number order.
    f_oneof = field("x", 1, T.TYPE_INT32, oneof_index=0)
    assert descr(f_oneof, "proto3") == ["::ppb::int32<F::x, ::ppb::field_semantics::always_lexn>"]

    f_plain = field("x", 1, T.TYPE_INT32)
    assert descr(f_plain, "proto3") == ["::ppb::proto3_int32<F::x>"]


def test_proto2_real_oneof_member_is_always_lexn():
    # Same in proto2: a real oneof member gets always_lexn, not the plain
    # last_write_wins of an ordinary proto2 optional scalar.
    f_oneof = field("x", 1, T.TYPE_INT32, oneof_index=0)
    assert descr(f_oneof, "proto2") == ["::ppb::int32<F::x, ::ppb::field_semantics::always_lexn>"]


def test_oneof_member_enum_string_bytes_are_always_lexn():
    enum_f = field("col", 3, T.TYPE_ENUM, type_name=".pkg.Color", oneof_index=0)
    assert descr(enum_f, "proto3") == [
        "::ppb::enumerated<F::col, ::pkg::Color, ::ppb::field_semantics::always_lexn>"
    ]

    str_f = field("text", 4, T.TYPE_STRING, oneof_index=0)
    assert descr(str_f, "proto3") == [
        "::ppb::utf8string<F::text, ::ppb::field_semantics::always_lexn>"
    ]

    bytes_f = field("blob", 5, T.TYPE_BYTES, oneof_index=0)
    assert descr(bytes_f, "proto3") == [
        "::ppb::bytes<F::blob, ::std::byte, ::ppb::field_semantics::always_lexn>"
    ]


def test_oneof_member_message_is_always_lexn():
    # A singular message oneof member: always_lexn replaces the singular
    # semantics it would otherwise have; the inner schema is always
    # merge_schema for singular fields.
    f = field("sub", 2, T.TYPE_MESSAGE, type_name=".pkg.Bar", oneof_index=0)
    assert descr(f, "proto3") == [
        "::ppb::message<F::sub, ::pkg::Bar::merge_schema, ::ppb::field_semantics::always_lexn>"
    ]

    merged = gen.map_field(
        f, syntax="proto3", strict_repeated_encoding=True, f_name="F", symbols=FakeSymbols()
    )
    assert merged == [
        "::ppb::message<F::sub, ::pkg::Bar::merge_schema, ::ppb::field_semantics::always_lexn>"
    ]


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


def test_repeated_message_uses_normal_schema():
    # A repeated TYPE_MESSAGE field always uses the plain schema: each occurrence
    # is a fresh element, so proto3 zero-default dispatch is safe on a fresh child.
    f = field("m", 1, T.TYPE_MESSAGE, label=T.LABEL_REPEATED, type_name=".pkg.Bar")
    result = gen.map_field(
        f, syntax="proto2", strict_repeated_encoding=True, f_name="F", symbols=FakeSymbols()
    )
    assert result == ["::ppb::unpacked_message<F::m, ::pkg::Bar::schema>"]


def test_always_dispatch_strings_forces_singular_on_strings():
    f = field("s", 3, T.TYPE_STRING)
    assert descr(f, "proto3", always_dispatch_strings=True) == [
        "::ppb::utf8string<F::s, ::ppb::field_semantics::singular>"
    ]
    # same for proto2
    assert descr(f, "proto2", always_dispatch_strings=True) == [
        "::ppb::utf8string<F::s, ::ppb::field_semantics::singular>"
    ]


def test_always_dispatch_strings_leaves_repeated_untouched():
    rep = field("s", 3, T.TYPE_STRING, label=T.LABEL_REPEATED)
    assert descr(rep, "proto3", always_dispatch_strings=True) == [
        "::ppb::unpacked_utf8string<F::s>"
    ]


def test_always_dispatch_strings_off_keeps_default():
    assert descr(field("s", 3, T.TYPE_STRING), "proto3") == ["::ppb::proto3_utf8string<F::s>"]
    assert descr(field("s", 3, T.TYPE_STRING), "proto2") == ["::ppb::utf8string<F::s>"]
