from google.protobuf import descriptor_pb2 as d
import protoc_gen_ppb as gen

from tests.conftest import make_file_proto

T = d.FieldDescriptorProto


def _model_with_keyword_field():
    foo = d.DescriptorProto(name="Foo")
    foo.field.add(name="int", number=1, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
    foo.field.add(name="ok", number=2, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
    fp = d.FileDescriptorProto(name="a.proto", package="pkg", syntax="proto3")
    fp.message_type.append(foo)
    return gen.build_model([fp])


def test_mangling_warnings_lists_keyword_identifiers():
    warns = gen.mangling_warnings(_model_with_keyword_field())
    assert any("'int'" in w and "'int_'" in w and ".pkg.Foo.int" in w for w in warns)
    assert all("'ok'" not in w for w in warns)


def test_mangling_warnings_cover_package_segments():
    # `package new.foo` -> namespace `new_::foo`; the keyword package
    # component must be warned about, not just message/field/enum names.
    foo = d.DescriptorProto(name="Bar")
    foo.field.add(name="x", number=1, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
    fp = d.FileDescriptorProto(name="a.proto", package="new.foo", syntax="proto3")
    fp.message_type.append(foo)
    warns = gen.mangling_warnings(gen.build_model([fp]))
    assert any("'new'" in w and "'new_'" in w and ".new" in w for w in warns)
    assert all("'foo'" not in w and "'Bar'" not in w for w in warns)


def test_no_warnings_without_keywords():
    foo = d.DescriptorProto(name="Foo")
    foo.field.add(name="ok", number=1, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
    fp = d.FileDescriptorProto(name="a.proto", package="pkg", syntax="proto3")
    fp.message_type.append(foo)
    assert gen.mangling_warnings(gen.build_model([fp])) == ()


def test_empty_message_warnings_one_per_empty_message():
    empty = d.DescriptorProto(name="Empty")
    fp = d.FileDescriptorProto(name="a.proto", package="pkg", syntax="proto3")
    fp.message_type.append(empty)
    warns = gen.empty_message_warnings(gen.build_model([fp]))
    assert len(warns) == 1
    assert "message has no fields" in warns[0]
    assert ".pkg.Empty" in warns[0]


def test_no_empty_message_warnings_when_all_have_fields():
    foo = d.DescriptorProto(name="Foo")
    foo.field.add(name="x", number=1, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
    fp = d.FileDescriptorProto(name="a.proto", package="pkg", syntax="proto3")
    fp.message_type.append(foo)
    assert gen.empty_message_warnings(gen.build_model([fp])) == ()


def test_drop_warnings_lists_groups_and_extension_ranges():
    m = d.DescriptorProto(name="Foo")
    m.field.add(name="g", number=2, type=T.TYPE_GROUP, label=T.LABEL_OPTIONAL, type_name=".pkg.G")
    er = m.extension_range.add()
    er.start = 120
    er.end = 201
    model = gen.build_model([make_file_proto(m)], skip_unsupported_fields=True)
    warnings = list(gen.drop_warnings(model))
    assert any("ppb-dropped: .pkg.Foo.g" in w for w in warnings)
    assert any("ppb-extension-range-ignored: 120-200" in w for w in warnings)


def test_drop_warnings_lists_extension_defs():
    fp = d.FileDescriptorProto(name="a.proto", package="pkg", syntax="proto2")
    other = fp.message_type.add(name="Other")
    other.field.add(name="x", number=1, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
    foo = fp.message_type.add(name="Foo")
    foo.extension.add(
        name="e", number=1000, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL, extendee=".pkg.Other"
    )
    model = gen.build_model([fp], skip_unsupported_fields=True)
    warnings = gen.drop_warnings(model)
    assert any("ppb-extension-def-ignored: .pkg.Foo.e" in w for w in warnings)


def test_generate_writes_warnings_to_stderr(capsys):
    from google.protobuf.compiler import plugin_pb2 as p

    req = p.CodeGeneratorRequest()
    fp = req.proto_file.add(name="a.proto", package="pkg", syntax="proto3")
    foo = fp.message_type.add(name="Foo")
    foo.field.add(name="int", number=1, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
    req.file_to_generate.append("a.proto")
    resp = gen.generate(req)
    assert resp.error == ""
    assert len(resp.file) == 1
    err = capsys.readouterr().err
    assert "C++ keyword 'int' emitted as 'int_'" in err


def test_required_downgrade_warns(capsys):
    from google.protobuf.compiler import plugin_pb2 as p

    req = p.CodeGeneratorRequest()
    fp = req.proto_file.add(name="reqwarn2.proto", package="reqwarn2", syntax="proto2")
    foo = fp.message_type.add(name="Foo")
    foo.field.add(name="bar", number=1, type=T.TYPE_INT32, label=T.LABEL_REQUIRED)
    req.file_to_generate.append("reqwarn2.proto")
    resp = gen.generate(req)
    assert resp.error == ""
    err = capsys.readouterr().err
    assert "required" in err
    assert "reqwarn2" in err
    assert "bar" in err


def test_default_value_warnings_lists_explicit_defaults():
    m = d.DescriptorProto(name="Foo")
    m.field.add(name="x", number=1, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
    y = d.FieldDescriptorProto(
        name="y", number=2, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL, default_value="42"
    )
    m.field.append(y)
    s = d.FieldDescriptorProto(
        name="s", number=3, type=T.TYPE_STRING, label=T.LABEL_OPTIONAL, default_value="hello"
    )
    m.field.append(s)
    fp = d.FileDescriptorProto(name="a.proto", package="pkg", syntax="proto2")
    fp.message_type.append(m)
    warns = gen.default_value_warnings(gen.build_model([fp]))
    assert len(warns) == 2
    assert any("default = '42'" in w and ".pkg.Foo.y" in w for w in warns)
    assert any("default = 'hello'" in w and ".pkg.Foo.s" in w for w in warns)
    assert all(".pkg.Foo.x" not in w for w in warns)


def test_default_value_warnings_labels_fields():
    m = d.DescriptorProto(name="Foo")
    m.field.add(name="r", number=1, type=T.TYPE_INT32, label=T.LABEL_REQUIRED, default_value="99")
    fp = d.FileDescriptorProto(name="a.proto", package="pkg", syntax="proto2")
    fp.message_type.append(m)
    warns = gen.default_value_warnings(gen.build_model([fp]))
    assert any("required" in w and "default = '99'" in w for w in warns)


def test_default_value_warnings_skips_proto3():
    m = d.DescriptorProto(name="Foo")
    m.field.add(name="x", number=1, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL, default_value="42")
    fp = d.FileDescriptorProto(name="a.proto", package="pkg", syntax="proto3")
    fp.message_type.append(m)
    warns = gen.default_value_warnings(gen.build_model([fp]))
    assert warns == ()


def test_default_value_warns_stderr(capsys):
    from google.protobuf.compiler import plugin_pb2 as p

    req = p.CodeGeneratorRequest()
    fp = req.proto_file.add(name="defwarn.proto", package="defwarn", syntax="proto2")
    foo = fp.message_type.add(name="Foo")
    foo.field.add(name="x", number=1, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
    foo.field.add(name="y", number=2, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL, default_value="42")
    req.file_to_generate.append("defwarn.proto")
    resp = gen.generate(req)
    assert resp.error == ""
    err = capsys.readouterr().err
    assert "default = '42'" in err
    assert "defwarn" in err
    assert "Foo" in err
    assert "y" in err
    assert "Foo.x: proto2" not in err


def test_oneof_as_optional_warns(capsys):
    from google.protobuf.compiler import plugin_pb2 as p

    req = p.CodeGeneratorRequest()
    req.parameter = "oneof_as_optional"
    fp = req.proto_file.add(name="oneof3.proto", package="pkg", syntax="proto3")
    has_oneof = fp.message_type.add(name="HasOneof")
    has_oneof.oneof_decl.add(name="choice")
    has_oneof.field.add(
        name="num", number=1, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL, oneof_index=0
    )
    has_oneof.field.add(
        name="other", number=2, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL, oneof_index=0
    )
    req.file_to_generate.append("oneof3.proto")
    resp = gen.generate(req)
    assert resp.error == ""
    err = capsys.readouterr().err
    assert "oneof" in err
    assert "exclusivity" in err
    assert "choice" in err
