from google.protobuf import descriptor_pb2 as d
import protoc_gen_ppb as gen

from tests.conftest import FakeSymbols

T = d.FieldDescriptorProto


def test_cpp_ident_appends_underscore_for_keywords():
    assert gen.cpp_ident("int") == "int_"
    assert gen.cpp_ident("class") == "class_"
    assert gen.cpp_ident("counts") == "counts"


def test_keyword_field_name_mangled_in_key():
    f = T(name="int", number=1, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
    got = gen.map_field(
        f, syntax="proto3", strict_repeated_encoding=True, f_name="F", symbols=FakeSymbols()
    )
    assert got == ["::ppb::proto3_int32<F::int_>"]


def test_keyword_message_name_mangled_in_namespace():
    assert gen._cpp_ns(".pkg.class.Inner") == "ppb_gen::pkg::class_::Inner"


def test_emit_message_mangles_keyword_field_enumerator():
    foo = d.DescriptorProto(name="Foo")
    foo.field.add(name="int", number=1, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
    fp = d.FileDescriptorProto(name="a.proto", package="pkg", syntax="proto3")
    fp.message_type.append(foo)
    model = gen.build_model([fp])
    text = gen.emit_message(
        model,
        model.message(".pkg.Foo"),
        strict_repeated_encoding=True,
        detect_unknown=False,
        opaque_fields=frozenset(),
    )
    assert "int_ = 1," in text
    assert "::ppb::proto3_int32<F::int_>" in text


def test_emit_enum_mangles_keyword_value_and_type_name():
    e = d.EnumDescriptorProto(name="enum")  # keyword type name
    e.value.add(name="default", number=0)  # keyword value name
    e.value.add(name="OK", number=1)
    foo = d.DescriptorProto(name="Foo", enum_type=[e])
    fp = d.FileDescriptorProto(name="a.proto", package="pkg", syntax="proto3")
    fp.message_type.append(foo)
    model = gen.build_model([fp])
    text = gen.emit_nested_enums(model.message(".pkg.Foo"))
    assert "enum class enum_ : ::std::int32_t" in text
    assert "default_ = 0," in text
