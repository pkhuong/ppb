from google.protobuf import descriptor_pb2 as d
import protoc_gen_ppb as gen

from tests.conftest import FakeSymbols, make_field

T = d.FieldDescriptorProto


def rep(name, number, ftype, type_name="", packed=None):
    return make_field(
        name, number, ftype, label=T.LABEL_REPEATED, type_name=type_name, packed=packed
    )


def descr(f, syntax, strict_repeated_encoding):
    return gen.map_field(
        f,
        syntax=syntax,
        strict_repeated_encoding=strict_repeated_encoding,
        f_name="F",
        symbols=FakeSymbols(),
    )


# Wire coverage is always both forms; the field's actual encoding is canonical
# and the fallback form depends on strict_repeated_encoding
# (field_semantics::error when True, always_lexn when False).


def test_none_proto3_packed_canonical_with_lexn_fallback():
    # proto3 int32 is packed by default, so packed is canonical and the unpacked
    # fallback is always_lexn (strict_repeated_encoding=False).
    assert descr(rep("v", 5, T.TYPE_INT32), "proto3", False) == [
        "::ppb::packed_int32<F::v>",
        "::ppb::int32<F::v, ::ppb::field_semantics::always_lexn>",
    ]


def test_full_proto3_packed_canonical_with_lexn_fallback():
    # strict_repeated_encoding=False gives the same lenient wire policy as 'none'.
    assert descr(rep("v", 5, T.TYPE_INT32), "proto3", False) == [
        "::ppb::packed_int32<F::v>",
        "::ppb::int32<F::v, ::ppb::field_semantics::always_lexn>",
    ]


def test_lean_proto3_packed_canonical_with_error_fallback():
    assert descr(rep("v", 5, T.TYPE_INT32), "proto3", True) == [
        "::ppb::packed_int32<F::v>",
        "::ppb::int32<F::v, ::ppb::field_semantics::error>",
    ]


def test_none_proto2_unpacked_canonical_with_lexn_fallback():
    # proto2 int32 is unpacked by default; the packed fallback is always_lexn.
    assert descr(rep("v", 5, T.TYPE_INT32), "proto2", False) == [
        "::ppb::unpacked_int32<F::v>",
        "::ppb::packed_int32<F::v, ::ppb::field_semantics::always_lexn>",
    ]


def test_lean_proto2_unpacked_canonical_with_error_fallback():
    assert descr(rep("v", 5, T.TYPE_INT32), "proto2", True) == [
        "::ppb::unpacked_int32<F::v>",
        "::ppb::packed_int32<F::v, ::ppb::field_semantics::error>",
    ]


def test_lean_obeys_explicit_packed_true_in_proto2():
    # explicit packed=true makes packed canonical;
    # strict_repeated_encoding=True makes the unpacked fallback error.
    assert descr(rep("v", 5, T.TYPE_INT32, packed=True), "proto2", True) == [
        "::ppb::packed_int32<F::v>",
        "::ppb::int32<F::v, ::ppb::field_semantics::error>",
    ]


def test_lean_obeys_explicit_packed_false_in_proto3():
    assert descr(rep("v", 5, T.TYPE_INT32, packed=False), "proto3", True) == [
        "::ppb::unpacked_int32<F::v>",
        "::ppb::packed_int32<F::v, ::ppb::field_semantics::error>",
    ]


def test_repeated_enum_uses_enumerated_variants():
    f = rep("c", 6, T.TYPE_ENUM, type_name=".pkg.Color")
    # proto3 enum packed by default; strict_repeated_encoding=False -> always_lexn fallback.
    assert descr(f, "proto3", False) == [
        "::ppb::packed_enumerated<F::c, ::pkg::Color>",
        "::ppb::enumerated<F::c, ::pkg::Color, ::ppb::field_semantics::always_lexn>",
    ]


def test_lean_repeated_enum_proto3_packed_canonical_error_fallback():
    f = rep("c", 6, T.TYPE_ENUM, type_name=".pkg.Color")
    assert descr(f, "proto3", True) == [
        "::ppb::packed_enumerated<F::c, ::pkg::Color>",
        "::ppb::enumerated<F::c, ::pkg::Color, ::ppb::field_semantics::error>",
    ]


def test_lean_repeated_enum_proto2_unpacked_canonical_error_fallback():
    f = rep("c", 6, T.TYPE_ENUM, type_name=".pkg.Color")
    # proto2 enum unpacked by default; the packed fallback must set sem in the 3rd
    # packed_enumerated arg.
    assert descr(f, "proto2", True) == [
        "::ppb::unpacked_enumerated<F::c, ::pkg::Color>",
        "::ppb::packed_enumerated<F::c, ::pkg::Color, ::ppb::field_semantics::error>",
    ]
