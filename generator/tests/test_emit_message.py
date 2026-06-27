from google.protobuf import descriptor_pb2 as d
import protoc_gen_ppb as gen

T = d.FieldDescriptorProto


def test_emit_simple_proto3_message():
    foo = d.DescriptorProto(name="Foo")
    foo.field.add(name="x", number=1, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
    foo.field.add(name="s", number=3, type=T.TYPE_STRING, label=T.LABEL_OPTIONAL)
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

    assert "namespace ppb_gen::pkg::Foo" in text
    assert "enum class F : ::std::int32_t" in text
    assert "x = 1," in text
    assert "s = 3," in text
    assert "constexpr ::std::size_t max_depth = 0;" in text
    assert "using schema = ::ppb::auto_schema<" in text
    assert "ppb::proto3_int32<F::x>" in text
    assert "ppb::proto3_utf8string<F::s>" in text


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


def test_detect_unknown_appends_catch_all():
    foo = d.DescriptorProto(name="Foo")
    foo.field.add(name="x", number=1, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
    fp = d.FileDescriptorProto(name="a.proto", package="pkg", syntax="proto3")
    fp.message_type.append(foo)
    model = gen.build_model([fp])
    text = gen.emit_message(
        model,
        model.message(".pkg.Foo"),
        strict_repeated_encoding=True,
        detect_unknown=True,
        opaque_fields=frozenset(),
    )
    assert "ppb::detect_unknown_fields<>" in text


def test_empty_message_registers_catch_all_with_comment():
    empty = d.DescriptorProto(name="Empty")
    fp = d.FileDescriptorProto(name="a.proto", package="pkg", syntax="proto3")
    fp.message_type.append(empty)
    model = gen.build_model([fp])
    text = gen.emit_message(
        model,
        model.message(".pkg.Empty"),
        strict_repeated_encoding=True,
        detect_unknown=False,
        opaque_fields=frozenset(),
    )
    assert "ppb::detect_unknown_fields<>" in text
    assert "ppb::auto_schema<>" not in text
    assert "presence/absence detection" in text
    assert "ppb::on<F::that_field>" in text
    assert "pkg.Empty declares no fields" in text


def test_empty_message_no_double_catch_all_under_detect_unknown():
    empty = d.DescriptorProto(name="Empty")
    fp = d.FileDescriptorProto(name="a.proto", package="pkg", syntax="proto3")
    fp.message_type.append(empty)
    model = gen.build_model([fp])
    text = gen.emit_message(
        model,
        model.message(".pkg.Empty"),
        strict_repeated_encoding=True,
        detect_unknown=True,
        opaque_fields=frozenset(),
    )
    # Both schema and merge_schema are identical (no fields), so merge_schema
    # is a type alias: detect_unknown_fields<> appears only once.
    assert text.count("ppb::detect_unknown_fields<>") == 1


def test_nonempty_lean_message_has_no_catch_all():
    foo = d.DescriptorProto(name="Foo")
    foo.field.add(name="x", number=1, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
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
    assert "ppb::detect_unknown_fields<>" not in text
    assert "declares no fields" not in text


def test_mangled_schema_name_used():
    schema_nested = d.DescriptorProto(name="schema")  # collides with `schema`
    foo = d.DescriptorProto(name="Foo", nested_type=[schema_nested])
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
    assert "using schema_ = ::ppb::auto_schema<" in text
