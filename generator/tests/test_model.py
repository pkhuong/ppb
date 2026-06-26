from google.protobuf import descriptor_pb2 as d
import protoc_gen_ppb as gen

from tests.conftest import make_file_proto

T = d.FieldDescriptorProto


def build(*files):
    return gen.build_model(list(files))


def msg(name, fields=(), nested=(), nested_enums=()):
    m = d.DescriptorProto(name=name)
    m.field.extend(fields)
    m.nested_type.extend(nested)
    m.enum_type.extend(nested_enums)
    return m


def enum(name, values):
    e = d.EnumDescriptorProto(name=name)
    for vn, vv in values:
        e.value.add(name=vn, number=vv)
    return e


def test_flatten_top_and_nested_messages():
    inner = msg("Bar", fields=[T(name="a", number=1, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)])
    outer = msg(
        "Foo",
        nested=[inner],
        fields=[
            T(
                name="b",
                number=2,
                type=T.TYPE_MESSAGE,
                label=T.LABEL_OPTIONAL,
                type_name=".pkg.Foo.Bar",
            )
        ],
    )
    model = build(make_file_proto(outer))
    names = {m.full_name for m in model.messages}
    assert names == {".pkg.Foo", ".pkg.Foo.Bar"}


def test_message_namespace_and_syntax():
    foo = msg("Foo")
    model = build(make_file_proto(foo, package="pkg.sub", syntax="proto2"))
    m = model.message(".pkg.sub.Foo")
    assert m.namespace == "ppb_gen::pkg::sub::Foo"
    assert m.syntax == "proto2"


def test_symbol_table_resolves_enum_and_message():
    color = enum("Color", [("RED", 0), ("GREEN", 1)])
    foo = msg("Foo", nested_enums=[color])
    model = build(make_file_proto(foo))
    assert model.symbols.enum_cpp_name(".pkg.Foo.Color") == "::ppb_gen::pkg::Foo::Color"
    assert model.symbols.message_schema(".pkg.Foo") == "::ppb_gen::pkg::Foo::schema"


def test_identifier_mangling_when_nested_type_named_F():
    f_msg = msg("F")  # nested message literally named F
    foo = msg("Foo", nested=[f_msg])
    model = build(make_file_proto(foo))
    assert model.message(".pkg.Foo").identifiers.f == "F_"
    # schema reference for Foo is unaffected
    assert model.symbols.message_schema(".pkg.Foo") == "::ppb_gen::pkg::Foo::schema"
