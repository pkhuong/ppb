from google.protobuf import descriptor_pb2 as d
import protoc_gen_ppb as gen

from tests.conftest import make_file_proto as file_with

T = d.FieldDescriptorProto


def map_entry(name, key_type, value_type, value_type_name=""):
    e = d.DescriptorProto(name=name)
    e.options.map_entry = True
    e.field.add(name="key", number=1, type=key_type, label=T.LABEL_OPTIONAL)
    v = e.field.add(name="value", number=2, type=value_type, label=T.LABEL_OPTIONAL)
    if value_type_name:
        v.type_name = value_type_name
    return e


def _with_map_field(entry):
    """Return a `Foo` message holding `entry` as a nested type and a repeated map field."""
    foo = d.DescriptorProto(name="Foo", nested_type=[entry])
    foo.field.add(
        name="m", number=1, type=T.TYPE_MESSAGE, label=T.LABEL_REPEATED, type_name=".pkg.Foo.MEntry"
    )
    return foo


def test_map_entry_modelled_as_message_and_field_is_unpacked_message():
    model = gen.build_model(
        [file_with(_with_map_field(map_entry("MEntry", T.TYPE_STRING, T.TYPE_INT32)))]
    )
    f = model.message(".pkg.Foo").fields[0]
    got = gen.map_field(
        f, syntax="proto3", strict_repeated_encoding=True, f_name="F", symbols=model.symbols
    )
    assert got == ["::ppb::unpacked_message<F::m, ::ppb_gen::pkg::Foo::MEntry::schema>"]


def test_map_field_identical_across_modes():
    # A map field is LEN-only, so every mode emits the same single descriptor.
    model = gen.build_model(
        [file_with(_with_map_field(map_entry("MEntry", T.TYPE_STRING, T.TYPE_INT32)))]
    )
    f = model.message(".pkg.Foo").fields[0]
    lean = gen.map_field(
        f, syntax="proto3", strict_repeated_encoding=True, f_name="F", symbols=model.symbols
    )
    none = gen.map_field(
        f, syntax="proto3", strict_repeated_encoding=False, f_name="F", symbols=model.symbols
    )
    full = gen.map_field(
        f, syntax="proto3", strict_repeated_encoding=False, f_name="F", symbols=model.symbols
    )
    assert lean == none == full


def test_proto3_map_entry_fields_use_zero_default_aliases():
    entry = map_entry("MEntry", T.TYPE_STRING, T.TYPE_INT32)
    foo = d.DescriptorProto(name="Foo", nested_type=[entry])
    model = gen.build_model([file_with(foo)])
    entry_msg = model.message(".pkg.Foo.MEntry")
    key = gen.map_field(
        entry_msg.fields[0],
        syntax="proto3",
        strict_repeated_encoding=True,
        f_name="F",
        symbols=model.symbols,
    )
    val = gen.map_field(
        entry_msg.fields[1],
        syntax="proto3",
        strict_repeated_encoding=True,
        f_name="F",
        symbols=model.symbols,
    )
    assert key == ["::ppb::proto3_utf8string<F::key>"]
    assert val == ["::ppb::proto3_int32<F::value>"]


def test_proto2_map_entry_fields_use_presence_aliases():
    # protoc lowers a proto2 map to an entry with optional key/value; proto2
    # has no zero-default widening, so the entry fields use the plain aliases
    # (utf8string / int32), not the proto3_* ones.
    entry = map_entry("MEntry", T.TYPE_STRING, T.TYPE_INT32)
    foo = d.DescriptorProto(name="Foo", nested_type=[entry])
    model = gen.build_model([file_with(foo, syntax="proto2")])
    entry_msg = model.message(".pkg.Foo.MEntry")
    key = gen.map_field(
        entry_msg.fields[0],
        syntax="proto2",
        strict_repeated_encoding=True,
        f_name="F",
        symbols=model.symbols,
    )
    val = gen.map_field(
        entry_msg.fields[1],
        syntax="proto2",
        strict_repeated_encoding=True,
        f_name="F",
        symbols=model.symbols,
    )
    assert key == ["::ppb::utf8string<F::key>"]
    assert val == ["::ppb::int32<F::value>"]


def test_message_valued_map_entry_references_value_schema():
    # map<string, Bar> -> entry value is a message field referencing Bar::schema.
    bar = d.DescriptorProto(name="Bar")
    bar.field.add(name="x", number=1, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
    entry = map_entry("MEntry", T.TYPE_STRING, T.TYPE_MESSAGE, value_type_name=".pkg.Bar")
    foo = d.DescriptorProto(name="Foo", nested_type=[entry])
    fp = file_with(foo)
    fp.message_type.append(bar)
    model = gen.build_model([fp])
    entry_msg = model.message(".pkg.Foo.MEntry")
    val = gen.map_field(
        entry_msg.fields[1],
        syntax="proto3",
        strict_repeated_encoding=True,
        f_name="F",
        symbols=model.symbols,
    )
    assert val == [
        "::ppb::message<F::value, ::ppb_gen::pkg::Bar::merge_schema, ::ppb::field_semantics::singular>"
    ]


def test_emit_file_defines_entry_namespace_before_outer_message():
    # The synthetic entry message gets its own namespace + schema, emitted
    # before the message whose map field references it.
    fp = file_with(_with_map_field(map_entry("MEntry", T.TYPE_STRING, T.TYPE_INT32)))
    model = gen.build_model([fp])
    text = gen.emit_file(
        model,
        fp,
        strict_repeated_encoding=True,
        detect_unknown=False,
        plan=gen.plan_emission(model, opaque_recursion=False),
    )
    assert "namespace ppb_gen::pkg::Foo::MEntry" in text
    assert text.index("namespace ppb_gen::pkg::Foo::MEntry") < text.index(
        "namespace ppb_gen::pkg::Foo\n"
    )
    assert "::ppb::unpacked_message<F::m, ::ppb_gen::pkg::Foo::MEntry::schema>" in text
    assert "::ppb::proto3_utf8string<F::key>" in text
