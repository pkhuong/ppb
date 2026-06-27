from google.protobuf import descriptor_pb2 as d
from google.protobuf.compiler import plugin_pb2 as p
import protoc_gen_ppb as gen

T = d.FieldDescriptorProto


def make_request(parameter=""):
    req = p.CodeGeneratorRequest()
    req.parameter = parameter
    fp = req.proto_file.add(name="a.proto", package="pkg", syntax="proto3")
    foo = fp.message_type.add(name="Foo")
    foo.field.add(name="x", number=1, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
    req.file_to_generate.append("a.proto")
    return req


def test_parse_options_defaults_to_lean_mode():
    opts = gen.parse_options("")
    assert opts.strict_repeated_encoding is True
    assert opts.detect_unknown is False
    assert opts.opaque_cycles is False


def test_parse_options_mode_none_and_full():
    assert gen.parse_options("mode=none").strict_repeated_encoding is False
    assert gen.parse_options("mode=full").strict_repeated_encoding is False


def test_parse_options_full_detect_unknown_opaque():
    opts = gen.parse_options("mode=full,detect_unknown,opaque_cycles")
    assert opts.strict_repeated_encoding is False
    assert opts.detect_unknown is True
    assert opts.opaque_cycles is True


def test_parse_options_oneof_as_optional():
    opts = gen.parse_options("oneof_as_optional")
    assert opts.oneof_as_optional is True
    assert opts.drop_foreign_type_fields is False


def test_parse_options_drop_foreign_type_fields():
    opts = gen.parse_options("drop_foreign_type_fields")
    assert opts.drop_foreign_type_fields is True
    assert opts.oneof_as_optional is False


def test_parse_options_defaults_off():
    opts = gen.parse_options("")
    assert opts.oneof_as_optional is False
    assert opts.drop_foreign_type_fields is False
    assert opts.drop_group_extension_fields is False


def test_parse_options_rejects_unknown():
    import pytest

    with pytest.raises(gen.GenError, match="unknown option"):
        gen.parse_options("bogus")


def test_generate_produces_header_file():
    resp = gen.generate(make_request())
    assert len(resp.file) == 1
    assert resp.file[0].name == "a.ppb.hpp"
    assert "namespace ppb_gen::pkg::Foo" in resp.file[0].content
    assert resp.supported_features & p.CodeGeneratorResponse.FEATURE_PROTO3_OPTIONAL


def make_two_file_request():
    req = p.CodeGeneratorRequest()
    a = req.proto_file.add(name="a.proto", package="pa", syntax="proto3")
    a.message_type.add(name="A").field.add(
        name="x", number=1, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL
    )
    b = req.proto_file.add(name="b.proto", package="pb", syntax="proto3")
    b.message_type.add(name="B").field.add(
        name="y", number=1, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL
    )
    req.file_to_generate.extend(["a.proto", "b.proto"])
    return req


def test_emission_graph_computed_once_per_request(monkeypatch):
    # The global emission order is identical for every output header, so a
    # multi-file request must build it once, not once per generated file.
    calls = 0
    real_emission_order = gen.emission_order

    def counting(*args, **kwargs):
        nonlocal calls
        calls += 1
        return real_emission_order(*args, **kwargs)

    monkeypatch.setattr(gen, "emission_order", counting)
    resp = gen.generate(make_two_file_request())
    assert len(resp.file) == 2
    assert calls == 1


def test_generate_sets_error_on_rejection():
    req = make_request()
    req.proto_file[0].message_type[0].field.add(
        name="g", number=2, type=T.TYPE_GROUP, label=T.LABEL_OPTIONAL, type_name=".pkg.G"
    )
    resp = gen.generate(req)
    assert "group" in resp.error
    assert len(resp.file) == 0


def test_generate_rejects_well_known_type_import():
    # A header for an *unsupported* WKT dependency is never generated, so its
    # #include and schema reference would dangle. The plugin rejects the import.
    # (Supported WKTs now redirect to the fused header instead.)
    req = make_request()
    req.proto_file[0].dependency.append("google/protobuf/struct.proto")
    req.proto_file.add(
        name="google/protobuf/struct.proto", package="google.protobuf"
    ).message_type.add(name="Struct")
    resp = gen.generate(req)
    assert "well-known type" in resp.error
    assert "google/protobuf/struct.proto" in resp.error
    assert len(resp.file) == 0


def test_generate_reports_unexpected_error(monkeypatch):
    # A non-GenError must surface as a structured response.error rather than
    # crashing the plugin process with a traceback on protoc's stderr.
    def boom(_proto_files, **_kwargs):
        raise ValueError("kaboom")

    monkeypatch.setattr(gen, "build_model", boom)
    resp = gen.generate(make_request())
    assert "internal error" in resp.error
    assert "kaboom" in resp.error
    assert len(resp.file) == 0


def _wire_varint(value):
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def _wire_len_field(number, payload):
    return _wire_varint((number << 3) | 2) + _wire_varint(len(payload)) + payload


def _deep_request_bytes(depth):
    # Hand-encoded CodeGeneratorRequest: the Python protobuf
    # serializer enforces the same nesting-depth limit as the parser,
    # so build the deeply nested wire bytes directly.
    # DescriptorProto: name=1, nested_type=3.
    msg = _wire_len_field(1, b"Leaf")
    for level in range(depth):
        msg = _wire_len_field(1, b"L%d" % level) + _wire_len_field(3, msg)

    # FileDescriptorProto: name=1, package=2, message_type=4, syntax=12.
    fp = (
        _wire_len_field(1, b"deep.proto")
        + _wire_len_field(2, b"patho")
        + _wire_len_field(4, msg)
        + _wire_len_field(12, b"proto3")
    )

    # CodeGeneratorRequest: file_to_generate=1, proto_file=15.
    return _wire_len_field(1, b"deep.proto") + _wire_len_field(15, fp)


def test_respond_decodes_and_generates():
    # Positive control: respond() is main()'s body and must round-trip a
    # well-formed request.
    resp = gen.respond(make_request().SerializeToString())
    assert not resp.error
    assert len(resp.file) == 1


def test_respond_decodes_deeply_nested_request():
    # 150 levels of inline nesting crash the default upb runtime's
    # request decode; the plugin selects the pure-Python runtime with
    # a raised recursion limit, so this must generate.
    resp = gen.respond(_deep_request_bytes(150))
    assert not resp.error
    assert len(resp.file) == 1


def test_respond_reports_undecodable_request():
    # Whatever still fails to decode (here: a LEN header pointing past
    # the end of the request bytes) must produce a structured
    # response.error, not a traceback.
    resp = gen.respond(b"\x0a\xff\xff\xff\xff\x7f")
    assert "CodeGeneratorRequest" in resp.error
    assert len(resp.file) == 0


def test_respond_reports_nesting_past_decode_limit():
    # Past the raised decode recursion limit, the error is still structured.
    resp = gen.respond(_deep_request_bytes(3000))
    assert "CodeGeneratorRequest" in resp.error
    assert len(resp.file) == 0
