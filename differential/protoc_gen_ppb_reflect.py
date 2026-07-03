#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["protobuf>=4"]
# ///
"""protoc-gen-ppb-reflect: emit the libprotobuf reflection sink for PPB.

This plugin emits code that parses protobuf bytes with PPB and writes
the decoded fields into a libprotobuf message via reflection
(`parse_into` / `merge_into`), for differential and conformance
testing.  It emits *only* the reflection-sink header
`foo.ppb.reflect.hpp`, which `#include`s the schema header generated
by regular (modulo some restricted flags) `protoc-gen-ppb`.
"""

import dataclasses
import os
import sys
import types

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "generator"))
# Reused unchanged from the production generator (this reflection sink is a
# parallel emitter over the same model).
from protoc_gen_ppb import (  # noqa: E402
    GenError,
    _T,
    _WKT_DEP_PREFIX,
    _WKT_SUPPORTED_FILES,
    _header_name,
    _inject_supported_wkt,
    _is_supported_wkt_with_fields,
    _missing_non_wkt_dependency,
    _request_from_descriptor_set,
    _write_response,
    build_model,
    compute_depths,
    cpp_ident,
    drop_warnings,
    emission_order,
    empty_message_warnings,
    find_back_edges,
    mangling_warnings,
    parse_options,
    plugin_pb2,
    resolve_identifier,
)

PLUGIN_NAME = "protoc-gen-ppb-reflect"


# proto scalar type -> (libprotobuf reflection method suffix, C++ handler arg
# type) for the sink.
_PB_SETTER = {
    _T.TYPE_INT32: ("Int32", "::std::int32_t"),
    _T.TYPE_SINT32: ("Int32", "::std::int32_t"),
    _T.TYPE_SFIXED32: ("Int32", "::std::int32_t"),
    _T.TYPE_INT64: ("Int64", "::std::int64_t"),
    _T.TYPE_SINT64: ("Int64", "::std::int64_t"),
    _T.TYPE_SFIXED64: ("Int64", "::std::int64_t"),
    _T.TYPE_UINT32: ("UInt32", "::std::uint32_t"),
    _T.TYPE_FIXED32: ("UInt32", "::std::uint32_t"),
    _T.TYPE_UINT64: ("UInt64", "::std::uint64_t"),
    _T.TYPE_FIXED64: ("UInt64", "::std::uint64_t"),
    _T.TYPE_BOOL: ("Bool", "bool"),
    _T.TYPE_FLOAT: ("Float", "float"),
    _T.TYPE_DOUBLE: ("Double", "double"),
}


# Default cross-message recursion budget; also the parse_into max_depth default.
_DEFAULT_MAX_RECURSION = 100


def _pb_fd(msg_expr, number):
    """C++ expression resolving field `number`'s FieldDescriptor on `msg_expr`."""
    return f"{msg_expr}->GetDescriptor()->FindFieldByNumber({number})"


def _pb_repeated_scalar_handler(field, key, msg_expr, suffix, arg_type) -> tuple:
    """Emit the on_each handler for a repeated numeric scalar.

    on_each<Key> fires once per decoded element across both wire forms
    (packed or unpacked).  The sink is semantics-agnostic: under
    `lean` the schema's error-semantics fallback rejects the
    non-canonical wire form at parse() before the handler runs.
    """
    fd = _pb_fd(msg_expr, field.number)
    refl = f"{msg_expr}->GetReflection()"
    body = f"{refl}->Add{suffix}({msg_expr}, {fd}, v);"
    return _pb_each_handler(key, f"{arg_type} v", body)


def _pb_handler(key, arg, body, *, wire=None):
    """One ::ppb::on<...> handler string with a single-statement lambda body."""
    on = f"::ppb::on<{key}>" if wire is None else f"::ppb::on<{key}, {wire}>"
    return f"{on}([&]({arg}) -> ppb_error {{ {body} return PPB_OK; }})"


def _pb_each_handler(key, arg, body):
    """One ::ppb::on_each<...> handler string with a single-statement lambda body.

    on_each fires `body` once per decoded element across both the packed and
    unpacked wire forms, normalizing fixed-width le_packed<T> elements to T.
    """
    return f"::ppb::on_each<{key}>([&]({arg}) -> ppb_error {{ {body} return PPB_OK; }})"


def _pb_reindent(text, base):
    """Prefix `base` spaces to every continuation line of `text` (not the first)."""
    pad = " " * base
    lines = text.split("\n")
    return "\n".join([lines[0]] + [pad + line for line in lines[1:]])


def _pb_place(items, base):
    """Emit handler strings as a comma-separated argument list."""
    pad = " " * base
    sep = ",\n" + pad
    return pad + sep.join(_pb_reindent(item, base) for item in items)


def _pb_string_value(field, src):
    """Build the ::std::string needed for a SetString/AddString.

    `src` is the decoded leaf value (a string_view for STRING, a byte span for BYTES).
    """
    if field.type == _T.TYPE_BYTES:
        return f"::std::string(reinterpret_cast<const char *>({src}.data()), {src}.size())"

    return f"::std::string({src})"


def _pb_repeated_enum_handler(field, key, msg_expr, enum_cpp) -> str:
    """Emit the on_each handler for a repeated enum."""
    fd = _pb_fd(msg_expr, field.number)
    refl = f"{msg_expr}->GetReflection()"
    add = f"{refl}->AddEnumValue({msg_expr}, {fd}, static_cast<int>(v));"
    return _pb_each_handler(key, f"{enum_cpp} v", add)


def _pb_leaf_handler(field, *, syntax, key, msg_expr, symbols) -> str:
    """Emit the ::ppb::on<> handler string for one non-message field, via reflection."""
    repeated = field.label == _T.LABEL_REPEATED
    fd = _pb_fd(msg_expr, field.number)
    refl = f"{msg_expr}->GetReflection()"

    if field.type == _T.TYPE_ENUM:
        enum_cpp = symbols.enum_cpp_name(field.type_name)
        if repeated:
            return _pb_repeated_enum_handler(field, key, msg_expr, enum_cpp)

        return _pb_handler(
            key,
            f"{enum_cpp} v",
            f"{refl}->SetEnumValue({msg_expr}, {fd}, static_cast<int>(v));",
        )

    if field.type == _T.TYPE_MESSAGE:
        raise NotImplementedError(
            f"libprotobuf sink: message field {field.name!r} is handled by emit_handlers"
        )

    if field.type in (_T.TYPE_STRING, _T.TYPE_BYTES):
        if field.type == _T.TYPE_STRING:
            arg = "::std::string_view sv"
            src = "sv"
        else:
            arg = "::std::span<const ::std::byte> s"
            src = "s"

        value = _pb_string_value(field, src)
        verb = "AddString" if repeated else "SetString"
        setter = f"{refl}->{verb}({msg_expr}, {fd}, {value});"

        # proto3 enforces well-formed UTF-8 on `string` (not `bytes`); proto2
        # does not.  Guard the setter so invalid UTF-8 aborts the parse.
        if field.type == _T.TYPE_STRING and syntax == "proto3":
            guard = f"if (!::ppb_gen::utf8::is_valid({src})) return PPB_ERROR_INVALID_UTF8; "
            setter = guard + setter

        return _pb_handler(key, arg, setter)

    if field.type in _PB_SETTER:
        suffix, arg_type = _PB_SETTER[field.type]
        if not repeated:
            return _pb_handler(key, f"{arg_type} v", f"{refl}->Set{suffix}({msg_expr}, {fd}, v);")

        return _pb_repeated_scalar_handler(field, key, msg_expr, suffix, arg_type)

    # Unreachable for valid input: groups/extensions are rejected during model
    # building, every other proto type is handled above.
    raise NotImplementedError(f"libprotobuf sink: field {field.name!r} type not handled")


def emit_handlers(
    model,
    message,
    *,
    msg_expr,
    depths,
    slots,
    opaque_fields,
    input_slots=None,
    merge=False,
    retain_unknown=False,
):
    """Emit ppb handler strings that write every field of `message` via reflection.

    The handlers write into the libprotobuf message named by the C++ expression `msg_expr`.

    Leaf fields go through `_pb_leaf_handler`.  A nested message field is
    inlined as a `::ppb::on_submessage<K, Inner::schema>` whose init creates the
    child via `MutableMessage` (singular) or `AddMessage` (repeated, incl. map
    entries).

    A cut back-edge field (in `opaque_fields`) is instead emitted as an opaque
    byte-span handler that calls `merge_into`  for a singular field and `parse_into`
    for a repeated one.

    Used both for a message's own `parse_into`/`merge_into` (msg_expr
    "msg") and inlined into a parent (msg_expr a slot).  When `merge`
    is True, generates `merge_into`, by making non-recursive nested
    message handlers use merge_schema and emitting inner handlers with
    merge=True.
    """
    if input_slots is None:
        input_slots = []

    ids = message.identifiers
    out = []
    for field in message.fields:
        key = f"::{message.namespace}::{ids.f}::{cpp_ident(field.name)}"
        if field.type == _T.TYPE_MESSAGE:
            if (message.full_name, field.name) in opaque_fields:
                out.append(
                    _emit_opaque_message_handler(
                        model,
                        field,
                        key=key,
                        msg_expr=msg_expr,
                    )
                )
            else:
                out.append(
                    _emit_submessage_handler(
                        model,
                        field,
                        key=key,
                        msg_expr=msg_expr,
                        depths=depths,
                        slots=slots,
                        input_slots=input_slots,
                        opaque_fields=opaque_fields,
                        merge=merge,
                        retain_unknown=retain_unknown,
                    )
                )
        else:
            out.append(
                _pb_leaf_handler(
                    field,
                    syntax=message.syntax,
                    key=key,
                    msg_expr=msg_expr,
                    symbols=model.symbols,
                )
            )

    return out


def _emit_opaque_message_handler(model, field, *, key, msg_expr):
    """Emit an opaque byte-span handler for a cut back-edge message field."""
    is_repeated = field.label == _T.LABEL_REPEATED
    verb = "AddMessage" if is_repeated else "MutableMessage"
    # A singular opaque back-edge (MutableMessage) reuses the child across wire
    # occurrences, so call merge_into.  A repeated opaque back-edge
    # (AddMessage) always gets a fresh element, so parse_into is safe.
    if not is_repeated:
        target = model.symbols.message_merge_into(field.type_name)
    else:
        target = model.symbols.message_parse_into(field.type_name)

    fd = _pb_fd(msg_expr, field.number)
    child = f"{msg_expr}->GetReflection()->{verb}({msg_expr}, {fd})"
    # This back-edge re-enters the target's parse_into/merge_into, recursing on
    # the native C++ stack once per nesting level.  The enclosing parse_into
    # threads a `::ppb::limit bounds` budget; we refuse to recurse once its depth
    # is exhausted and otherwise hand the target a budget decremented by one, so
    # adversarial deep nesting yields PPB_ERROR_DEPTH_EXCEEDED rather than a stack
    # overflow.
    return (
        f"::ppb::on<{key}>([&](::std::span<const ::std::byte> s) -> ppb_error {{ "
        f"if (bounds.depth() == 0) return PPB_ERROR_DEPTH_EXCEEDED; "
        f"return {target}(s, {child}, bounds.with_max_depth(bounds.depth() - 1)); }})"
    )


def _emit_submessage_handler(
    model,
    field,
    *,
    key,
    msg_expr,
    depths,
    slots,
    input_slots,
    opaque_fields,
    merge=False,
    retain_unknown=False,
):
    """Emit a nested message field as an inlined ::ppb::on_submessage handler."""
    verb = "AddMessage" if field.label == _T.LABEL_REPEATED else "MutableMessage"
    is_repeated = field.label == _T.LABEL_REPEATED

    inner = model.message(field.type_name)
    # A singular field (MutableMessage) reuses the child across wire
    # occurrences, so it must use merge_schema and recurse with merge=True so
    # absent proto3 scalars do not clobber existing values.  A repeated field
    # (AddMessage) gets a fresh element per occurrence, so proto3 zero-default
    # is safe and we use the plain schema with merge=False.
    use_merge = not is_repeated
    retain_inner = retain_unknown and not inner.map_entry
    wkt_strict = retain_inner and _is_supported_wkt_with_fields(model.symbols, field.type_name)
    if use_merge:
        inner_schema = (
            model.symbols.message_merge_schema_with_unknowns(field.type_name)
            if wkt_strict
            else model.symbols.message_merge_schema(field.type_name)
        )
    else:
        inner_schema = (
            model.symbols.message_schema_with_unknowns(field.type_name)
            if wkt_strict
            else model.symbols.message_schema(field.type_name)
        )

    fd = _pb_fd(msg_expr, field.number)
    # Map entries never retain (libprotobuf drops entry unknowns when it
    # syncs the repeated representation into the map), but the reference
    # parser still rejects invalid field numbers there, so we register
    # validate-only unknown handlers instead.
    validate_inner = retain_unknown and inner.map_entry
    needs_input = retain_inner or validate_inner

    if not inner.fields and not needs_input:
        # An empty submessage has no child handlers; just create it to mark the
        # occurrence (no hoisted slot, which would otherwise be set-but-unused).
        init = (
            f"[&](const ::ppb::reader<{inner_schema}> &) -> ppb_error {{ "
            f"{msg_expr}->GetReflection()->{verb}({msg_expr}, {fd}); return PPB_OK; }}"
        )
        return f"::ppb::on_submessage<{key}, {inner_schema}>(\n{_pb_place([init], 4)})"

    slot = f"m{len(slots)}"
    in_slot = f"in{slot[1:]}"

    slots.append(slot)
    if needs_input:
        input_slots.append(in_slot)
        init = (
            f"[&](const ::ppb::reader<{inner_schema}> &r) -> ppb_error {{ "
            f"{slot} = {msg_expr}->GetReflection()->{verb}({msg_expr}, {fd}); "
            f"{in_slot} = r.input(); return PPB_OK; }}"
        )
    else:
        init = (
            f"[&](const ::ppb::reader<{inner_schema}> &) -> ppb_error {{ "
            f"{slot} = {msg_expr}->GetReflection()->{verb}({msg_expr}, {fd}); return PPB_OK; }}"
        )

    inner_handlers = emit_handlers(
        model,
        inner,
        msg_expr=slot,
        depths=depths,
        slots=slots,
        input_slots=input_slots,
        opaque_fields=opaque_fields,
        merge=use_merge,
        retain_unknown=retain_unknown,
    )
    if retain_inner:
        inner_handlers.extend(_retain_unknown_handlers_deferred(slot, in_slot))
    elif validate_inner:
        inner_handlers.extend(_validate_unknown_handlers_deferred(in_slot))

    body = _pb_place([init, *inner_handlers], 4)
    return f"::ppb::on_submessage<{key}, {inner_schema}>(\n{body})"


def _retain_unknown_handlers(msg_expr, input_expr):
    """Emit the four on_unknown retain handlers for a toplevel scope.

    `input_expr` is a C++ expression yielding the original input span so that
    `retain_unknown_field` can clamp the tag-redecode buffer to the actual
    remaining bytes.
    """
    out = []
    for w in ("varint", "i64", "len", "i32"):
        out.append(
            f"::ppb::on_unknown<::ppb::wire_type::{w}>("
            f"[&, input = {input_expr}](const ppb_field &f) -> ppb_error {{ "
            f"return ::ppb_sink::retain_unknown_field<::ppb::wire_type::{w}>(f, {msg_expr}, input); }})"
        )

    return out


def _retain_unknown_handlers_deferred(msg_expr, input_var):
    """Emit the deferred on_unknown retain handlers for an inlined submessage scope.

    Like `_retain_unknown_handlers`, but `msg_expr` and `input_var` are hoisted
    locals that the on_submessage init assigns when the submessage is entered,
    *after* the handlers are constructed.
    """
    out = []
    for w in ("varint", "i64", "len", "i32"):
        out.append(
            f"::ppb::on_unknown<::ppb::wire_type::{w}>("
            f"[&](const ppb_field &f) -> ppb_error {{ "
            f"return ::ppb_sink::retain_unknown_field<::ppb::wire_type::{w}>"
            f"(f, {msg_expr}, {input_var}); }})"
        )

    return out


def _validate_unknown_handlers_deferred(input_var):
    """Emit validate-only unknown handlers for inlined map-entry scopes.

    They reject invalid field numbers (zero, reachable through an overlong
    tag encoding, or above 2^29-1).
    """
    out = []
    for w in ("varint", "i64", "len", "i32"):
        out.append(
            f"::ppb::on_unknown<::ppb::wire_type::{w}>("
            f"[&](const ppb_field &f) -> ppb_error {{ "
            f"return ::ppb_sink::validate_unknown_field(f, {input_var}); }})"
        )

    return out


def _emit_one_parse_fn(
    model,
    message,
    *,
    fn_name,
    schema_name,
    depths,
    opaque_fields,
    retain_unknown,
    merge,
    out,
):
    """Append to `out` the reader-taking and span-taking overloads of `fn_name`.

    `fn_name` is "parse_into" or "merge_into"; `schema_name` is the C++
    schema alias name (ids.schema or ids.merge_schema); `merge` controls
    whether the emitted handlers suppress absent proto3 scalars.
    """
    default_depth = max(_DEFAULT_MAX_RECURSION, depths[message.full_name])
    bounds_def = f", ::ppb::limit bounds = ::ppb::limit::max_depth({default_depth})"

    out.append("    inline ppb_error")
    out.append(
        f"    {fn_name}(::ppb::reader<{schema_name}> &reader, "
        f"::google::protobuf::Message *msg{bounds_def})"
    )
    out.append("    {")
    if not message.fields:
        init = f"[&](const ::ppb::reader<{schema_name}> &) -> ppb_error {{ (void) msg; return PPB_OK; }}"
        # Canonical order is parse(init, limit, handlers...): the parse(limit,
        # init, ...) overload requires at least one trailing handler, which a
        # fieldless lean message does not have.
        args = [init, "bounds"]
        if retain_unknown:
            args.extend(_retain_unknown_handlers("msg", "reader.input()"))

        out.append(f"        return reader.parse(\n{_pb_place(args, 12)});")
    else:
        slots = []
        input_slots = []
        handlers = emit_handlers(
            model,
            message,
            msg_expr="msg",
            depths=depths,
            slots=slots,
            input_slots=input_slots,
            opaque_fields=opaque_fields,
            merge=merge,
            retain_unknown=retain_unknown,
        )
        for slot in slots:
            out.append(f"        ::google::protobuf::Message *{slot} = nullptr;")

        for in_slot in input_slots:
            out.append(f"        ::std::span<const ::std::byte> {in_slot};")

        if slots:
            out.append("")

        args = ["bounds", *handlers]
        if retain_unknown:
            args.extend(_retain_unknown_handlers("msg", "reader.input()"))

        out.append(f"        return reader.parse(\n{_pb_place(args, 12)});")

    out.append("    }")
    out.append("")

    out.append("    inline ppb_error")
    out.append(
        f"    {fn_name}(::std::span<const ::std::byte> bytes, "
        f"::google::protobuf::Message *msg{bounds_def})"
    )
    out.append("    {")
    out.append(f"        ::ppb::reader<{schema_name}> reader(bytes);")
    out.append(f"        return {fn_name}(reader, msg, bounds);")
    out.append("    }")


def emit_parse_into(
    model,
    message,
    *,
    depths,
    opaque_fields,
    retain_unknown=False,
):
    """Emit the libprotobuf sink namespace block for one message.

    Always emits a reader-taking `parse_into` (hoisted Message* slots,
    optional max_depth budget, inlined handler list) plus a span convenience
    overload, and a `merge_into` pair using merge_schema and merge-mode
    handlers (proto3 implicit-presence scalars use last_write_wins so absent
    scalars are not dispatched).
    """
    ids = message.identifiers
    out = [f"namespace {message.namespace}", "{"]

    _emit_one_parse_fn(
        model,
        message,
        fn_name=ids.parse_into,
        schema_name=ids.schema,
        depths=depths,
        opaque_fields=opaque_fields,
        retain_unknown=retain_unknown,
        merge=False,
        out=out,
    )

    out.append("")
    _emit_one_parse_fn(
        model,
        message,
        fn_name=ids.merge_into,
        schema_name=ids.merge_schema,
        depths=depths,
        opaque_fields=opaque_fields,
        retain_unknown=retain_unknown,
        merge=True,
        out=out,
    )

    out.append("}")
    out.append("")
    return "\n".join(out)


@dataclasses.dataclass(frozen=True)
class EmissionPlan:
    """Whole-closure emission data shared by every file in a request."""
    order: tuple  # messages in emission order
    opaque_fields: frozenset
    depths: types.MappingProxyType  # message full_name -> max_depth (read-only)


def plan_emission(model, *, opaque_recursion):
    back = find_back_edges(model)
    opaque_fields = back if opaque_recursion else frozenset()
    return EmissionPlan(
        order=emission_order(model, opaque_recursion=opaque_recursion, back=back),
        opaque_fields=opaque_fields,
        depths=types.MappingProxyType(compute_depths(model, opaque_fields=opaque_fields)),
    )


def _reflect_header_name(proto_file_name):
    # "sub/a.proto" -> "sub/a.ppb.reflect.hpp"
    if not proto_file_name.endswith(".proto"):
        raise GenError(f"expected a .proto file name, got {proto_file_name!r}")

    return proto_file_name.removesuffix(".proto") + ".ppb.reflect.hpp"


def _emit_parse_into_forward_decl(message):
    """Forward-declare a message's span `parse_into` and `merge_into` overloads."""
    ids = message.identifiers
    lines = [
        f"namespace {message.namespace}",
        "{",
        "    inline ppb_error",
        f"    {ids.parse_into}(::std::span<const ::std::byte> bytes, "
        "::google::protobuf::Message *msg, ::ppb::limit bounds);",
        "    inline ppb_error",
        f"    {ids.merge_into}(::std::span<const ::std::byte> bytes, "
        "::google::protobuf::Message *msg, ::ppb::limit bounds);",
        "}",
        "",
    ]
    return "\n".join(lines)


def emit_libprotobuf_file(
    model,
    file_proto,
    *,
    plan,
    skip_wkt=False,
    retain_unknown=False,
):
    """Emit the `<base>.ppb.reflect.hpp` sink header for one file."""
    own = {m.full_name for m in model.messages if m.source_file == file_proto.name}

    out = [
        "#pragma once",
        "",
        "// clang-format off",
        f'#include "{_header_name(file_proto.name)}"',
        "",
        "#include <span>",
        "",
        "#include <google/protobuf/message.h>",
        '#include "ppb_utf8.hpp"',
    ]
    if retain_unknown:
        out.append('#include "ppb_libprotobuf_unknown.hpp"')

    for dep in file_proto.dependency:
        if dep.startswith(_WKT_DEP_PREFIX):
            if dep in _WKT_SUPPORTED_FILES:
                # A supported WKT dep needs no special include: the schema headers
                # directly include the necessary headers.
                continue

            if skip_wkt:
                continue

            raise GenError(
                f"{file_proto.name!r} imports well-known type {dep!r}, which "
                f"protoc-gen-ppb does not support; pass "
                f"--ppb_opt=drop_foreign_type_fields to drop fields that "
                f"reference it (those fields will not be decoded)"
            )

        out.append(f'#include "{_reflect_header_name(dep)}"')

    out.append("")

    # Back-edge handlers introduce circular dependencies.
    # Forward-declare their targets at the top so the call resolves
    # regardless of where the target is defined in this file.
    cut_targets = {
        field.type_name
        for message in plan.order
        if message.full_name in own
        for field in message.fields
        if (message.full_name, field.name) in plan.opaque_fields
    }
    for message in plan.order:
        if message.full_name in own and message.full_name in cut_targets:
            out.append(_emit_parse_into_forward_decl(message))

    for message in plan.order:
        if message.full_name in own:
            out.append(
                emit_parse_into(
                    model,
                    message,
                    depths=plan.depths,
                    opaque_fields=plan.opaque_fields,
                    retain_unknown=retain_unknown,
                )
            )

    return "\n".join(out) + "\n"


def _map_entry_full_names(proto_files):
    """Proto full names (".pkg.Foo.BarEntry") of synthetic map-entry messages.

    libprotobuf models a `map<K, V>` field as a repeated synthetic *Entry
    message carrying the DescriptorProto.options.map_entry flag; the sink
    must AddMessage a fresh entry per occurrence and must not retain unknowns
    inside it.  build_model does not surface that flag, so recover it directly
    from the descriptors.
    """
    names = set()

    def walk(prefix, desc):
        full = f"{prefix}.{desc.name}"
        if desc.options.map_entry:
            names.add(full)

        for nested in desc.nested_type:
            walk(full, nested)

    for fp in proto_files:
        base = f".{fp.package}" if fp.package else ""
        for desc in fp.message_type:
            walk(base, desc)

    return names


def _sibling_type_names(model, message):
    """Reconstruct a message's nested type/enum names for identifier resolution."""
    prefix = message.full_name + "."
    names = set()
    for other in model.messages:
        if other.full_name.startswith(prefix) and "." not in other.full_name[len(prefix) :]:
            names.add(other.full_name[len(prefix) :])

    for enum in message.enums:
        names.add(enum.full_name.rsplit(".", 1)[1])

    return frozenset(names)


def _enrich_model(model, *, map_entry_names):
    """Decorate production's model with the reflection-sink-only attributes.

    Adds `parse_into` / `merge_into` to each MessageIdentifiers, `map_entry` to
    each Message, and `message_parse_into` / `message_merge_into` to the
    SymbolTable, so we can mostly use the production model.
    """
    for message in model.messages:
        taken = _sibling_type_names(model, message)
        ids = message.identifiers
        object.__setattr__(ids, "parse_into", resolve_identifier("parse_into", taken=taken))
        object.__setattr__(ids, "merge_into", resolve_identifier("merge_into", taken=taken))
        object.__setattr__(message, "map_entry", message.full_name in map_entry_names)

    symbols = model.symbols

    def message_parse_into(proto_full_name, _symbols=symbols):
        m = _symbols.message(proto_full_name)
        return f"::{m.namespace}::{m.identifiers.parse_into}"

    def message_merge_into(proto_full_name, _symbols=symbols):
        m = _symbols.message(proto_full_name)
        return f"::{m.namespace}::{m.identifiers.merge_into}"

    symbols.message_parse_into = message_parse_into
    symbols.message_merge_into = message_merge_into


def generate(request):
    response = plugin_pb2.CodeGeneratorResponse()
    response.supported_features = plugin_pb2.CodeGeneratorResponse.FEATURE_PROTO3_OPTIONAL

    try:
        opts = parse_options(request.parameter)
        model = build_model(
            list(request.proto_file),
            allow_oneof=opts.oneof_as_optional,
            skip_wkt=opts.drop_foreign_type_fields,
            files_to_generate=request.file_to_generate,
            skip_unsupported_fields=opts.drop_group_extension_fields,
        )
        _enrich_model(model, map_entry_names=_map_entry_full_names(request.proto_file))
        for w in mangling_warnings(model):
            sys.stderr.write(w + "\n")

        for w in empty_message_warnings(model):
            sys.stderr.write(w + "\n")

        for w in drop_warnings(model):
            sys.stderr.write(w + "\n")

        plan = plan_emission(model, opaque_recursion=opts.opaque_cycles)
        retain_unknown = opts.detect_unknown
        by_name = {fp.name: fp for fp in request.proto_file}
        for name in request.file_to_generate:
            fp = by_name[name]
            # The reflect plugin emits *only* the reflection sink; the
            # production protoc-gen-ppb (run alongside) emits the schema header
            # this sink #includes.
            refl = response.file.add()
            refl.name = _reflect_header_name(name)
            refl.content = emit_libprotobuf_file(
                model,
                fp,
                plan=plan,
                skip_wkt=opts.drop_foreign_type_fields,
                retain_unknown=retain_unknown,
            )
    except GenError as exc:
        response.ClearField("file")
        response.error = str(exc)
    except Exception as exc:
        # protoc would otherwise surface a bare "plugin crashed" with a Python
        # traceback on its stderr; a structured response.error is friendlier.
        response.ClearField("file")
        response.error = f"internal error: {exc}"

    return response


def respond(data: bytes):
    try:
        request = plugin_pb2.CodeGeneratorRequest.FromString(data)
    except Exception as exc:
        # A structured error beats a traceback on protoc's stderr.
        response = plugin_pb2.CodeGeneratorResponse()
        response.error = f"cannot decode CodeGeneratorRequest: {exc}"
        return response

    return generate(request)


def _run_standalone(argv):
    import argparse

    parser = argparse.ArgumentParser(prog=PLUGIN_NAME)
    parser.add_argument("--descriptor-set")
    parser.add_argument("--opt", default="")
    parser.add_argument("--out", default=".")
    parser.add_argument("files", nargs="*")
    args = parser.parse_args(argv)

    if not args.descriptor_set:
        parser.error("--descriptor-set is required")

    request = _request_from_descriptor_set(args.descriptor_set, args.files, args.opt)
    _inject_supported_wkt(request)
    missing = _missing_non_wkt_dependency(request)
    if missing is not None:
        sys.stderr.write(
            f"{PLUGIN_NAME}: dependency {missing!r} is missing from the descriptor "
            f"set; regenerate it with protoc --include_imports\n"
        )
        return 1

    response = generate(request)
    if response.error:
        sys.stderr.write(f"{PLUGIN_NAME}: {response.error}\n")
        return 1

    return _write_response(response, args.out)


def main() -> int:
    if len(sys.argv) > 1:
        return _run_standalone(sys.argv[1:])

    data = sys.stdin.buffer.read()
    response = respond(data)
    sys.stdout.buffer.write(response.SerializeToString())
    return 0


if __name__ == "__main__":
    sys.exit(main())
