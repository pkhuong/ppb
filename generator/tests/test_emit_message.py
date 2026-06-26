from google.protobuf import descriptor_pb2 as d
import protoc_gen_ppb as gen

T = d.FieldDescriptorProto


def test_emit_nested_enum_and_enumerated_field():
    color = d.EnumDescriptorProto(name="Color")
    color.value.add(name="RED", number=0)
    color.value.add(name="GREEN", number=1)
    foo = d.DescriptorProto(name="Foo", enum_type=[color])
    foo.field.add(
        name="c", number=2, type=T.TYPE_ENUM, label=T.LABEL_OPTIONAL, type_name=".pkg.Foo.Color"
    )
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
    # The nested enum *definition* is emitted separately (see emit_nested_enums);
    # emit_message only has the field-key enum and the descriptor list.
    assert "enum class Color" not in text
    assert "ppb::proto3_enumerated<F::c, ::ppb_gen::pkg::Foo::Color>" in text

    enums = gen.emit_nested_enums(model.message(".pkg.Foo"))
    assert "namespace ppb_gen::pkg::Foo\n{\n    enum class Color : ::std::int32_t" in enums
    assert "RED = 0," in enums
