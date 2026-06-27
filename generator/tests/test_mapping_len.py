from google.protobuf import descriptor_pb2 as d
import protoc_gen_ppb as gen

from tests.conftest import FakeSymbols, make_field as f

T = d.FieldDescriptorProto


def descr(fd, syntax, strict_repeated_encoding=True):
    return gen.map_field(
        fd,
        syntax=syntax,
        strict_repeated_encoding=strict_repeated_encoding,
        f_name="F",
        symbols=FakeSymbols(),
    )


def test_string_singular():
    assert descr(f("s", 1, T.TYPE_STRING), "proto2") == ["::ppb::utf8string<F::s>"]
    assert descr(f("s", 1, T.TYPE_STRING), "proto3") == ["::ppb::proto3_utf8string<F::s>"]
    assert descr(f("s", 1, T.TYPE_STRING, proto3_optional=True), "proto3") == [
        "::ppb::utf8string<F::s>"
    ]


def test_bytes_singular():
    assert descr(f("b", 2, T.TYPE_BYTES), "proto3") == ["::ppb::proto3_bytes<F::b>"]


def test_string_bytes_repeated_always_unpacked_both_modes():
    rs = f("s", 1, T.TYPE_STRING, label=T.LABEL_REPEATED)
    assert descr(rs, "proto3", True) == ["::ppb::unpacked_utf8string<F::s>"]
    assert descr(rs, "proto3", False) == ["::ppb::unpacked_utf8string<F::s>"]
    rb = f("b", 2, T.TYPE_BYTES, label=T.LABEL_REPEATED)
    assert descr(rb, "proto2", "full") == ["::ppb::unpacked_bytes<F::b>"]
