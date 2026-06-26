#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["protobuf>=4"]
# ///
"""protoc-gen-ppb: emit ppb::schema headers from a .proto descriptor closure."""

import dataclasses
import os
import sys

# Force the pure python (instead of upb) protobuf runtime to enable parsing
# heavily nested schemas: upb craps out on schemas that protoc will happily
# generate.  We still have the interpreter and protobuf recursion limits,
# so it's not like we're accepting unbounded recursion either.
os.environ["PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION"] = "python"

from google.protobuf import descriptor_pb2
from google.protobuf.internal import decoder as _pb_decoder

# Request decode, build_model, emission_order, and compute_depths all
# recurse over the message graph; protoc imposes no depth limit, so a
# pathologically deep closure could blow Python's default 1000-frame
# ceiling.
sys.setrecursionlimit(max(sys.getrecursionlimit(), 10_000))

# pyrefly: ignore[missing-attribute]  # absent from the stubs, present at runtime
if hasattr(_pb_decoder, "SetRecursionLimit"):
    _pb_decoder.SetRecursionLimit(2_000)

# Every generated message/enum namespace nests under this dedicated toplevel
# namespace.  A proto `package pkg` with message `Msg` becomes
# `ppb_gen::pkg::Msg`.  It is a *sibling* of the `ppb` library namespace, not
# nested inside it: we don't awnt proto packages named `schema`/`reader`/etc.
# to introduce collisions.
_GEN_NS = "ppb_gen"

# A field whose message/enum type lives under this proto package references a
# well-known type. --ppb_opt=drop_foreign_type_fields drops such fields.
_WKT_TYPE_PREFIX = ".google.protobuf."

_T = descriptor_pb2.FieldDescriptorProto

# C++ keywords (and a few context-sensitive identifiers) that can't be used
# verbatim as identifiers in generated code. Matches protobuf's cpp kKeywords
# approach: a colliding identifier gets a single trailing underscore.
_CPP_KEYWORDS = frozenset(
    {
        "alignas",
        "alignof",
        "and",
        "and_eq",
        "asm",
        "auto",
        "bitand",
        "bitor",
        "bool",
        "break",
        "case",
        "catch",
        "char",
        "char8_t",
        "char16_t",
        "char32_t",
        "class",
        "compl",
        "concept",
        "const",
        "consteval",
        "constexpr",
        "constinit",
        "const_cast",
        "continue",
        "co_await",
        "co_return",
        "co_yield",
        "decltype",
        "default",
        "delete",
        "do",
        "double",
        "dynamic_cast",
        "else",
        "enum",
        "explicit",
        "export",
        "extern",
        "false",
        "float",
        "for",
        "friend",
        "goto",
        "if",
        "inline",
        "int",
        "long",
        "mutable",
        "namespace",
        "new",
        "noexcept",
        "not",
        "not_eq",
        "nullptr",
        "operator",
        "or",
        "or_eq",
        "private",
        "protected",
        "public",
        "register",
        "reinterpret_cast",
        "requires",
        "return",
        "short",
        "signed",
        "sizeof",
        "static",
        "static_assert",
        "static_cast",
        "struct",
        "switch",
        "template",
        "this",
        "thread_local",
        "throw",
        "true",
        "try",
        "typedef",
        "typeid",
        "typename",
        "union",
        "unsigned",
        "using",
        "virtual",
        "void",
        "volatile",
        "wchar_t",
        "while",
        "xor",
        "xor_eq",
        "NULL",
    }
)


def cpp_ident(name):
    """Return a proto identifier as a valid C++ identifier (keyword -> name + '_')."""
    return name + "_" if name in _CPP_KEYWORDS else name


def resolve_identifier(name, *, taken):
    """Append underscores until `name` is free of the `taken` set."""
    while name in taken:
        name += "_"

    return name


@dataclasses.dataclass(frozen=True)
class MessageIdentifiers:
    f: str
    schema: str
    merge_schema: str
    max_depth: str


def resolve_message_identifiers(*, sibling_type_names):
    """Resolve every injected name against sibling types.

    Covers the schema header's F / schema / merge_schema / max_depth.
    A nested type of the same name becomes a C++ namespace in the same scope,
    which is ill-formed ("redeclared as a different kind of entity"), so
    we try to find the identifiers as they were renamed.
    """
    taken = frozenset(sibling_type_names)
    f = resolve_identifier("F", taken=taken)
    schema = resolve_identifier("schema", taken=taken)
    merge_schema = resolve_identifier("merge_schema", taken=taken)
    max_depth = resolve_identifier("max_depth", taken=taken)
    return MessageIdentifiers(
        f=f,
        schema=schema,
        merge_schema=merge_schema,
        max_depth=max_depth,
    )


@dataclasses.dataclass(frozen=True)
class DroppedField:
    name: str  # the proto field name
    full_name: str  # ".pkg.Foo.dur"
    reason: str  # human-readable reason; emitted as a C++ comment in the generated header


@dataclasses.dataclass(frozen=True)
class EnumValue:
    name: str
    number: int


@dataclasses.dataclass(frozen=True)
class Enum:
    full_name: str  # ".pkg.Foo.Color"
    cpp_name: str  # "::pkg::Foo::Color"
    values: tuple[EnumValue, ...]
    source_file: str = ""  # defining .proto; set for file-scope enums


@dataclasses.dataclass(frozen=True)
class Message:
    full_name: str  # ".pkg.Foo.Bar"
    namespace: str  # "pkg::Foo::Bar"
    syntax: str  # "proto2" | "proto3"
    # Read-only protoc handles, never copied or mutated; FieldDescriptorProto isn't
    # hashable, so neither is Message.
    fields: tuple[descriptor_pb2.FieldDescriptorProto, ...]
    enums: tuple[Enum, ...]  # nested, direct
    identifiers: MessageIdentifiers
    decl_index: int  # declaration order across the closure
    source_file: str  # proto file name that defined this message
    dropped_fields: tuple[DroppedField, ...] = ()
    ignored_extension_ranges: tuple[tuple[int, int], ...] = ()
    ignored_extension_defs: tuple[str, ...] = ()
    # Names of user-declared oneofs in this message.  Non-empty in practice only
    # when allow_oneof=True; validation otherwise rejects real oneofs before
    # build_model returns.
    real_oneof_names: tuple[str, ...] = ()


class SymbolTable:
    def __init__(self):
        self._messages = {}  # proto full name -> Message
        self._enums = {}  # proto full name -> Enum

    def add_message(self, m):
        self._messages[m.full_name] = m

    def add_enum(self, e):
        self._enums[e.full_name] = e

    def has_message(self, name):
        return name in self._messages

    def has_enum(self, name):
        return name in self._enums

    def message(self, proto_full_name):
        return self._messages[proto_full_name]

    def enum_cpp_name(self, proto_full_name):
        return self._enums[proto_full_name].cpp_name

    def message_schema(self, proto_full_name):
        m = self._messages[proto_full_name]
        return f"::{m.namespace}::{m.identifiers.schema}"

    def message_merge_schema(self, proto_full_name):
        m = self._messages[proto_full_name]
        return f"::{m.namespace}::{m.identifiers.merge_schema}"

    def message_schema_with_unknowns(self, proto_full_name):
        # schemas may have both a regular version and an extended
        # version with unknowns, get the latter from a proto's name.
        m = self._messages[proto_full_name]
        return f"::{m.namespace}::{m.identifiers.schema}_with_unknowns"

    def message_merge_schema_with_unknowns(self, proto_full_name):
        m = self._messages[proto_full_name]
        return f"::{m.namespace}::{m.identifiers.merge_schema}_with_unknowns"


@dataclasses.dataclass(frozen=True)
class Model:
    messages: tuple[Message, ...]
    symbols: SymbolTable
    file_enums: tuple[Enum, ...] = ()  # file-scope (top-level) enums across the closure

    def message(self, proto_full_name):
        return self.symbols.message(proto_full_name)


class GenError(Exception):
    """Raised for inputs protoc-gen-ppb deliberately rejects."""


def _cpp_ns(proto_full_name):
    """Convert a proto-qualified name to a C++ namespace.

    ".pkg.Foo.Bar" -> "ppb_gen::pkg::Foo::Bar", keyword components
    -> "..._". The empty proto name (toplevel, no package) maps to the
    bare _GEN_NS.
    """
    parts = [cpp_ident(p) for p in proto_full_name.strip(".").split(".") if p]
    return "::".join([_GEN_NS, *parts])


def _check_no_mangling_collisions(scope, names):
    """`names` is an iterable of raw proto identifiers in one C++ scope.

    Raise GenError if two distinct names collide after cpp_ident().
    """
    seen = {}
    for name in names:
        ident = cpp_ident(name)
        if ident in seen and seen[ident] != name:
            raise GenError(
                f"{scope}: colliding identifiers: {seen[ident]!r} and {name!r} both map to "
                f"{ident!r} after C++-keyword mangling"
            )
        seen[ident] = name


def _validate_message(desc, full, *, allow_oneof, skip_unsupported_fields=False):
    if (desc.extension_range or desc.extension) and not skip_unsupported_fields:
        raise GenError(
            f"{full}: extension ranges/definitions are unsupported; pass "
            f"--ppb_opt=drop_group_extension_fields to ignore them (extension "
            f"fields on the wire are treated as unknown fields)"
        )

    # Real oneofs: a oneof_decl with a non-synthetic member. proto3 `optional`
    # creates a synthetic single-member oneof; drop those before complaining.
    synthetic = {
        f.oneof_index for f in desc.field if f.proto3_optional and f.HasField("oneof_index")
    }
    used = {f.oneof_index for f in desc.field if f.HasField("oneof_index")}
    if (used - synthetic) and not allow_oneof:
        raise GenError(
            f"{full}: oneof is unsupported; pass --ppb_opt=oneof_as_optional to "
            f"decode each member as an independent last_write_wins field (this "
            f"loses oneof exclusivity at the schema level)"
        )


# A proto field number is a 29-bit positive integer: 1 .. 2**29 - 1.  protoc
# checks for that already, but defence in depth doesn't hurt.
_MAX_FIELD_NUMBER = (1 << 29) - 1

# Field numbers 19000..19999 are reserved by protobuf for the implementation.
_RESERVED_FIELD_NUMBER_LO = 19000
_RESERVED_FIELD_NUMBER_HI = 19999


def _validate_field_numbers(full, fields):
    """Reject illegal or duplicate field numbers within one message."""
    seen = {}
    for f in fields:
        if f.number < 1 or f.number > _MAX_FIELD_NUMBER:
            raise GenError(
                f"{full}.{f.name}: field number {f.number} is out of range "
                f"(must be 1..{_MAX_FIELD_NUMBER})"
            )

        if _RESERVED_FIELD_NUMBER_LO <= f.number <= _RESERVED_FIELD_NUMBER_HI:
            raise GenError(
                f"{full}.{f.name}: field number {f.number} is in the "
                f"protobuf-reserved range "
                f"{_RESERVED_FIELD_NUMBER_LO}..{_RESERVED_FIELD_NUMBER_HI}"
            )

        if f.number in seen:
            raise GenError(
                f"{full}: duplicate field number {f.number} ({seen[f.number]!r} and {f.name!r})"
            )

        seen[f.number] = f.name


def _validate_syntax(fp):
    """Return the normalized syntax ("proto2"/"proto3") for a file, rejecting Protobuf Editions.

    We implement proto2 or proto3 rules. Editions are different, and we're not
    sure we implement any of them correctly, so fail closed for now.
    """
    if fp.syntax == "editions" or fp.HasField("edition"):
        raise GenError(f"{fp.name!r}: Protobuf Editions is unsupported (only proto2/proto3)")

    norm = fp.syntax or "proto2"
    if norm not in ("proto2", "proto3"):
        raise GenError(f"unsupported syntax {fp.syntax!r} (only proto2/proto3)")

    return norm


def _build_enum(ed, parent_full, source_file=""):
    full = f"{parent_full}.{ed.name}"
    return Enum(
        full_name=full,
        cpp_name="::" + _cpp_ns(full),
        values=tuple(EnumValue(v.name, v.number) for v in ed.value),
        source_file=source_file,
    )


def build_model(
    proto_files,
    *,
    allow_oneof=False,
    skip_unsupported_fields=False,
):
    symbols = SymbolTable()
    messages = []
    file_enums = []
    counter = [0]
    package_scope_names = {}  # package scope -> toplevel type names across files

    def walk_message(desc, parent_full, syntax, source_file):
        full = f"{parent_full}.{desc.name}"
        _validate_message(
            desc, full, allow_oneof=allow_oneof, skip_unsupported_fields=skip_unsupported_fields
        )
        _validate_field_numbers(full, desc.field)
        nested_enums = []
        sibling_type_names = {n.name for n in desc.nested_type} | {e.name for e in desc.enum_type}
        _check_no_mangling_collisions(
            full,
            list(n.name for n in desc.nested_type) + list(e.name for e in desc.enum_type),
        )
        _check_no_mangling_collisions(full, (f.name for f in desc.field))
        for ed in desc.enum_type:
            e = _build_enum(ed, full)
            _check_no_mangling_collisions(e.full_name, (v.name for v in ed.value))
            nested_enums.append(e)
            symbols.add_enum(e)

        kept = []
        dropped = []
        for f in desc.field:
            refs_wkt = f.type in (_T.TYPE_MESSAGE, _T.TYPE_ENUM) and f.type_name.startswith(
                _WKT_TYPE_PREFIX
            )

            if f.type == _T.TYPE_GROUP:
                if skip_unsupported_fields:
                    dropped.append(
                        DroppedField(
                            name=f.name,
                            full_name=f"{full}.{f.name}",
                            reason="group field (wire types 3/4 are undecodable)",
                        )
                    )
                else:
                    raise GenError(
                        f"{full}.{f.name}: group field uses wire types 3/4, which "
                        f"PPB cannot decode; pass --ppb_opt=drop_group_extension_fields "
                        f"to drop it (payloads that exercise it will fail to parse)"
                    )

            elif refs_wkt:
                raise GenError(
                    f"{full}.{f.name} references well-known type {f.type_name}, "
                    f"which protoc-gen-ppb does not support; pass "
                    f"--ppb_opt=drop_foreign_type_fields to drop fields that "
                    f"reference it (those fields will not be decoded)"
                )

            else:
                kept.append(f)

        ignored_ranges = (
            tuple((er.start, er.end - 1) for er in desc.extension_range)
            if skip_unsupported_fields
            else ()
        )
        ignored_defs = (
            tuple(f"{full}.{ext.name}" for ext in desc.extension) if skip_unsupported_fields else ()
        )
        identifiers = resolve_message_identifiers(sibling_type_names=frozenset(sibling_type_names))
        decl_index = counter[0]
        counter[0] += 1
        synthetic_oneof = {
            f.oneof_index for f in desc.field if f.proto3_optional and f.HasField("oneof_index")
        }
        used_oneof = {f.oneof_index for f in desc.field if f.HasField("oneof_index")}
        real_oneof_indices = sorted(used_oneof - synthetic_oneof)
        for i in real_oneof_indices:
            if not 0 <= i < len(desc.oneof_decl):
                raise GenError(
                    f"{full}: field references oneof index {i}, but the message "
                    f"declares only {len(desc.oneof_decl)} oneof(s)"
                )

        real_oneof_names = tuple(desc.oneof_decl[i].name for i in real_oneof_indices)
        m = Message(
            full_name=full,
            namespace=_cpp_ns(full),
            syntax=syntax,
            fields=tuple(kept),
            enums=tuple(nested_enums),
            identifiers=identifiers,
            decl_index=decl_index,
            source_file=source_file,
            dropped_fields=tuple(dropped),
            ignored_extension_ranges=ignored_ranges,
            ignored_extension_defs=ignored_defs,
            real_oneof_names=real_oneof_names,
        )
        symbols.add_message(m)
        messages.append(m)
        for nested in desc.nested_type:
            walk_message(nested, full, syntax, source_file)

    for fp in proto_files:
        syntax = _validate_syntax(fp)
        base = f".{fp.package}" if fp.package else ""
        scope = base or "<toplevel>"
        # Toplevel names share one C++ namespace per *package*, which several
        # files may contribute to; accumulate and check across the whole
        # closure after the loop, not per file.
        package_scope_names.setdefault(scope, []).extend(
            [desc.name for desc in fp.message_type] + [ed.name for ed in fp.enum_type]
        )
        for ed in fp.enum_type:
            e = _build_enum(ed, base, fp.name)
            _check_no_mangling_collisions(e.full_name, (v.name for v in ed.value))
            symbols.add_enum(e)
            file_enums.append(e)

        for desc in fp.message_type:
            walk_message(desc, base, syntax, fp.name)

    for scope, names in package_scope_names.items():
        _check_no_mangling_collisions(scope, names)

    for m in messages:
        for f in m.fields:
            if f.type == _T.TYPE_MESSAGE and not symbols.has_message(f.type_name):
                raise GenError(
                    f"{m.full_name}.{f.name}: references undefined message type {f.type_name}"
                )

            if f.type == _T.TYPE_ENUM and not symbols.has_enum(f.type_name):
                raise GenError(
                    f"{m.full_name}.{f.name}: references undefined enum type {f.type_name}"
                )

    return Model(messages=tuple(messages), symbols=symbols, file_enums=tuple(file_enums))
