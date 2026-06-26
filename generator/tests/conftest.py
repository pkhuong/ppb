"""Shared test helpers: a SymbolTable stub plus field/file-proto factories.

Imported instead of injected as fixtures because most calls don't happen in
test bodies.
"""

import os

# Must be set before the first google.protobuf import anywhere in the
# test process so the suite runs the same protobuf runtime the plugin
# selects for itself (pure Python; see the protoc_gen_ppb.py preamble).
os.environ["PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION"] = "python"

from google.protobuf import descriptor_pb2 as d
import protoc_gen_ppb as gen

T = d.FieldDescriptorProto


class FakeSymbols:
    """SymbolTable stub: resolve proto type names to C++ names, keyword-aware.

    `enum_cpp_name` mangles each path segment through `cpp_ident`, so it is a
    superset of the plain ".".replace("::") form (non-keyword names are
    unchanged) and works for keyword-bearing inputs too.
    """

    def enum_cpp_name(self, proto_type_name):
        # ".pkg.Foo.Color" -> "::pkg::Foo::Color"
        return "::" + "::".join(gen.cpp_ident(p) for p in proto_type_name.strip(".").split("."))

    def message_schema(self, proto_type_name):
        # ".pkg.Bar" -> "::pkg::Bar::schema"
        return self.enum_cpp_name(proto_type_name) + "::schema"

    def message_merge_schema(self, proto_type_name):
        # ".pkg.Bar" -> "::pkg::Bar::merge_schema"
        return self.enum_cpp_name(proto_type_name) + "::merge_schema"


def make_field(
    name,
    number,
    ftype,
    label=T.LABEL_OPTIONAL,
    type_name="",
    proto3_optional=False,
    packed=None,
    oneof_index=None,
):
    f = T(name=name, number=number, type=ftype, label=label)
    if type_name:
        f.type_name = type_name

    if proto3_optional:
        f.proto3_optional = True

    if packed is not None:
        f.options.packed = packed

    if oneof_index is not None:
        f.oneof_index = oneof_index

    return f


def make_file_proto(*messages, name="a.proto", package="pkg", syntax="proto3", enums=()):
    fp = d.FileDescriptorProto(name=name, package=package, syntax=syntax)
    fp.message_type.extend(messages)
    fp.enum_type.extend(enums)
    return fp
