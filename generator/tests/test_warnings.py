from google.protobuf import descriptor_pb2 as d
import protoc_gen_ppb as gen

T = d.FieldDescriptorProto


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
