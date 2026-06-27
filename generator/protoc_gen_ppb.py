#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["protobuf>=4"]
# ///
"""protoc-gen-ppb: emit ppb::schema headers from a .proto descriptor closure."""

import argparse
import dataclasses
import heapq
import os
import sys
import types

# Force the pure python (instead of upb) protobuf runtime to enable parsing
# heavily nested schemas: upb craps out on schemas that protoc will happily
# generate.  We still have the interpreter and protobuf recursion limits,
# so it's not like we're accepting unbounded recursion either.
os.environ["PROTOCOL_BUFFERS_PYTHON_IMPLEMENTATION"] = "python"

from google.protobuf import descriptor_pb2
from google.protobuf.compiler import plugin_pb2
from google.protobuf.internal import decoder as _pb_decoder

# Request decode, build_model, emission_order, and compute_depths all
# recurse over the message graph; protoc imposes no depth limit, so a
# pathologically deep closure could blow Python's default 1000-frame
# ceiling.
sys.setrecursionlimit(max(sys.getrecursionlimit(), 10_000))

# pyrefly: ignore[missing-attribute]  # absent from the stubs, present at runtime
if hasattr(_pb_decoder, "SetRecursionLimit"):
    _pb_decoder.SetRecursionLimit(2_000)

PLUGIN_NAME = "protoc-gen-ppb"

# Every generated message/enum namespace nests under this dedicated toplevel
# namespace.  A proto `package pkg` with message `Msg` becomes
# `ppb_gen::pkg::Msg`.  It is a *sibling* of the `ppb` library namespace, not
# nested inside it: we don't awnt proto packages named `schema`/`reader`/etc.
# to introduce collisions.
_GEN_NS = "ppb_gen"

# Well-known types live under this import path. We don't generate headers for
# them (they're not in file_to_generate), so a generated `#include` and any
# `::google::protobuf::Foo::schema` reference would fail. Reject up front.
_WKT_DEP_PREFIX = "google/protobuf/"

# A field whose message/enum type lives under this proto package references a
# well-known type. --ppb_opt=drop_foreign_type_fields drops such fields.
_WKT_TYPE_PREFIX = ".google.protobuf."

# The trivial well-known types natively supported by protoc-gen-ppb.
# Their schemas are prebuilt in <ppb/wkt.ppb.hpp> (generated with
# --emit-wkt-bundle), so a field referencing one resolves like any
# cross-file message and its import redirects to that single fused
# header.
#
# Other WKTs, like struct/type/api/source_context/descriptor are
# rejected.
_WKT_SUPPORTED_FILES = frozenset(
    {
        "google/protobuf/any.proto",
        "google/protobuf/duration.proto",
        "google/protobuf/empty.proto",
        "google/protobuf/field_mask.proto",
        "google/protobuf/timestamp.proto",
        "google/protobuf/wrappers.proto",
    }
)

# Module import paths for the six supported WKT files' generated _pb2 modules,
# keyed by proto file name.
_WKT_PB2_MODULES = {
    "google/protobuf/any.proto": "google.protobuf.any_pb2",
    "google/protobuf/duration.proto": "google.protobuf.duration_pb2",
    "google/protobuf/empty.proto": "google.protobuf.empty_pb2",
    "google/protobuf/field_mask.proto": "google.protobuf.field_mask_pb2",
    "google/protobuf/timestamp.proto": "google.protobuf.timestamp_pb2",
    "google/protobuf/wrappers.proto": "google.protobuf.wrappers_pb2",
}

# Fixed alphabetical fusion order (the six files don't import
# anything, so concatenation in any total order is valid; alphabetical
# is stable).
_WKT_BUNDLE_ORDER = (
    "google/protobuf/any.proto",
    "google/protobuf/duration.proto",
    "google/protobuf/empty.proto",
    "google/protobuf/field_mask.proto",
    "google/protobuf/timestamp.proto",
    "google/protobuf/wrappers.proto",
)


_T = descriptor_pb2.FieldDescriptorProto

# proto field type -> ppb base descriptor name (packable numeric scalars + enum).
_SCALAR_BASE = {
    _T.TYPE_INT32: "int32",
    _T.TYPE_INT64: "int64",
    _T.TYPE_UINT32: "uint32",
    _T.TYPE_UINT64: "uint64",
    _T.TYPE_SINT32: "sint32",
    _T.TYPE_SINT64: "sint64",
    _T.TYPE_BOOL: "boolean",
    _T.TYPE_FIXED32: "fixed32",
    _T.TYPE_SFIXED32: "sfixed32",
    _T.TYPE_FLOAT: "f32",
    _T.TYPE_FIXED64: "fixed64",
    _T.TYPE_SFIXED64: "sfixed64",
    _T.TYPE_DOUBLE: "f64",
}

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


def _is_proto3_zero_default(field, syntax):
    """Determine whether a field is a proto3 scalar with zero-defaults.

    Real oneof members need explicit presence (the oneof itself), so
    zero-default dispatch must NOT fire for them: dispatching an absent member
    would overwrite the oneof case that was set on the wire.
    """
    return syntax == "proto3" and not field.proto3_optional and not field.HasField("oneof_index")


_ALWAYS_LEXN = "::ppb::field_semantics::always_lexn"
_ERROR = "::ppb::field_semantics::error"


def _is_real_oneof_member(field):
    """Member of a user-declared oneof (not a proto3 `optional` synthetic oneof).

    Only reachable under --ppb_opt=oneof_as_optional (real oneofs are otherwise
    rejected during model-building).  Such members are emitted with always_lexn
    semantics so that a present member forces a wire-order lexn pass.
    """
    return field.HasField("oneof_index") and not field.proto3_optional


def _singular_scalar(field, syntax, key, merge=False):
    base = _SCALAR_BASE[field.type]
    if _is_real_oneof_member(field):
        return [f"::ppb::{base}<{key}, {_ALWAYS_LEXN}>"]

    # In merge mode, treat the field as last_write_wins (no proto3_ prefix) so
    # absent scalars are NOT dispatched and do not clobber values written by a
    # previous wire occurrence.
    use_proto3 = _is_proto3_zero_default(field, syntax) and not merge
    name = "proto3_" + base if use_proto3 else base
    return [f"::ppb::{name}<{key}>"]


def _singular_enum(field, syntax, key, symbols, merge=False):
    enum_cpp = symbols.enum_cpp_name(field.type_name)
    if _is_real_oneof_member(field):
        return [f"::ppb::enumerated<{key}, {enum_cpp}, {_ALWAYS_LEXN}>"]

    use_proto3 = _is_proto3_zero_default(field, syntax) and not merge
    name = "proto3_enumerated" if use_proto3 else "enumerated"
    return [f"::ppb::{name}<{key}, {enum_cpp}>"]


def _field_is_packed(field, syntax):
    if field.HasField("options") and field.options.HasField("packed"):
        return field.options.packed

    return syntax == "proto3"


def _repeated_variants(base, key, args, *, strict_repeated_encoding, is_packed):
    """`args` is "" for scalars or ", <Enum>" for enums.

    Always emits both wire forms: the field's actual encoding is canonical
    (repeated, prescan fast-path eligible).  The other (fallback) form's
    semantics depend on `strict_repeated_encoding`:

      - True: field_semantics::error, so any occurrence of the unexpected
        encoding fails closed at parse()
      - False: always_lexn, so its mere presence forces the order-preserving
        lexn pass, so a both-forms message decodes in wire order (otherwise,
        a field split across both forms with <= 1 occurrence each dispatches
        in tag order, not wire order)
    """
    fallback_sem = _ERROR if strict_repeated_encoding else _ALWAYS_LEXN
    packed = f"::ppb::packed_{base}<{key}{args}>"
    unpacked = f"::ppb::unpacked_{base}<{key}{args}>"
    if is_packed:
        unpacked_fallback = f"::ppb::{base}<{key}{args}, {fallback_sem}>"
        return [packed, unpacked_fallback]

    if base == "enumerated":
        enum = args[len(", ") :]
        packed_fallback = f"::ppb::packed_enumerated<{key}, {enum}, {fallback_sem}>"
    else:
        packed_fallback = f"::ppb::packed_{base}<{key}, {fallback_sem}>"

    return [unpacked, packed_fallback]


def _is_supported_wkt_with_fields(symbols, type_name):
    """Determine whether a type is a WKT message with fields.

    Unknown-field detection wraps such a schema in
    ppb::auto_schema<..., ppb::detect_unknown_fields<>> at the use
    site (on the schema-side message<> descriptor).

    Not needed for the Empty WKT, since we always emit catch-alls
    for empty schemas.
    """
    if not symbols.has_message(type_name):
        return False

    msg = symbols.message(type_name)
    return msg.source_file in _WKT_SUPPORTED_FILES and bool(msg.fields)


def _message_inner_alias(field, symbols, *, detect_unknown, repeated):
    """Return the qualified inner-alias string for a non-opaque TYPE_MESSAGE field.

    Shared by map_field (descriptor emission) and _field_handler_hint (hint
    comment): picks merge_schema for singular fields (repeated occurrences merge
    into one child, so absent proto3 scalars must not clobber existing values)
    and schema for repeated (each occurrence gets a fresh element, so proto3
    zero-default is safe).

    Adds _with_unknowns for supported WKT, when `detect_unknown` is true.
    """
    wkt_strict = detect_unknown and _is_supported_wkt_with_fields(symbols, field.type_name)
    if not repeated:
        return (
            symbols.message_merge_schema_with_unknowns(field.type_name)
            if wkt_strict
            else symbols.message_merge_schema(field.type_name)
        )
    return (
        symbols.message_schema_with_unknowns(field.type_name)
        if wkt_strict
        else symbols.message_schema(field.type_name)
    )


def map_field(
    field,
    *,
    syntax,
    strict_repeated_encoding,
    f_name,
    symbols,
    opaque=False,
    merge=False,
    detect_unknown=False,
):
    """Return the list of ppb descriptor strings for one proto field.

    `syntax` is "proto2" or "proto3"; `strict_repeated_encoding` controls
    whether unexpected repeated wire encodings fail closed (True) or force
    a wire-order lexn pass (False); `f_name` is the resolved field-key enum
    name; `symbols` resolves message/enum type names to fully-qualified C++
    names.  When `opaque` is True and the field is TYPE_MESSAGE, the field
    is rendered as a raw byte span (ppb::bytes / ppb::unpacked_bytes)
    instead of a typed message descriptor; used for back-edges under
    --ppb_opt=opaque_cycles.  When `merge` is True (used for generating
    merge_schema), proto3 implicit-presence scalars use the non-proto3_
    (last_write_wins) variant so absent scalars are NOT dispatched.
    """
    key = f"{f_name}::{cpp_ident(field.name)}"
    repeated = field.label == _T.LABEL_REPEATED

    if not repeated and field.type == _T.TYPE_ENUM:
        return _singular_enum(field, syntax, key, symbols, merge=merge)

    if not repeated and field.type in _SCALAR_BASE:
        return _singular_scalar(field, syntax, key, merge=merge)

    if repeated and field.type == _T.TYPE_ENUM:
        is_packed = _field_is_packed(field, syntax)
        return _repeated_variants(
            "enumerated",
            key,
            f", {symbols.enum_cpp_name(field.type_name)}",
            strict_repeated_encoding=strict_repeated_encoding,
            is_packed=is_packed,
        )

    if repeated and field.type in _SCALAR_BASE:
        is_packed = _field_is_packed(field, syntax)
        return _repeated_variants(
            _SCALAR_BASE[field.type],
            key,
            "",
            strict_repeated_encoding=strict_repeated_encoding,
            is_packed=is_packed,
        )

    if field.type == _T.TYPE_STRING:
        if repeated:
            return [f"::ppb::unpacked_utf8string<{key}>"]

        if _is_real_oneof_member(field):
            return [f"::ppb::utf8string<{key}, {_ALWAYS_LEXN}>"]

        use_proto3 = _is_proto3_zero_default(field, syntax) and not merge
        name = "proto3_utf8string" if use_proto3 else "utf8string"
        return [f"::ppb::{name}<{key}>"]

    if field.type == _T.TYPE_BYTES:
        if repeated:
            return [f"::ppb::unpacked_bytes<{key}>"]

        if _is_real_oneof_member(field):
            return [f"::ppb::bytes<{key}, ::std::byte, {_ALWAYS_LEXN}>"]

        use_proto3 = _is_proto3_zero_default(field, syntax) and not merge
        name = "proto3_bytes" if use_proto3 else "bytes"
        return [f"::ppb::{name}<{key}>"]

    if field.type == _T.TYPE_MESSAGE:
        if opaque:
            if repeated:
                return [f"::ppb::unpacked_bytes<{key}> /* recursive: opaque */"]

            if _is_real_oneof_member(field):
                return [f"::ppb::bytes<{key}, ::std::byte, {_ALWAYS_LEXN}> /* recursive: opaque */"]

            # Opaque singular: occurrences merge into one child, so use singular semantics.
            return [
                f"::ppb::bytes<{key}, ::std::byte, ::ppb::field_semantics::singular> /* recursive: opaque */"
            ]

        # The `merge` kwarg controls this message's own scalar/enum/string/bytes
        # semantics (see _message_inner_alias), not nested submessages.
        inner = _message_inner_alias(
            field,
            symbols,
            detect_unknown=detect_unknown,
            repeated=repeated,
        )

        if repeated:
            return [f"::ppb::unpacked_message<{key}, {inner}>"]

        if _is_real_oneof_member(field):
            return [f"::ppb::message<{key}, {inner}, {_ALWAYS_LEXN}>"]

        return [f"::ppb::message<{key}, {inner}, ::ppb::field_semantics::singular>"]

    # Unreachable for valid input: groups and extensions are rejected during
    # model-building, and every other proto type/label is handled above.
    raise NotImplementedError(f"field {field.name!r} type/label not handled yet")


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


def _supported_wkt_type_names(proto_files):
    """Return full proto names of messages/enums declared in supported WKT files.

    Covers every supported WKT file present in the closure
    (e.g. ".google.protobuf.Timestamp"). Used to tell whether a WKT field is
    supported. Tries to handle nested types, though we don't support any for now.
    """
    names = set()

    def walk(desc, parent_full):
        full = f"{parent_full}.{desc.name}"
        names.add(full)
        for e in desc.enum_type:
            names.add(f"{full}.{e.name}")
        for nested in desc.nested_type:
            walk(nested, full)

    for fp in proto_files:
        if fp.name not in _WKT_SUPPORTED_FILES:
            continue

        base = f".{fp.package}" if fp.package else ""
        for e in fp.enum_type:
            names.add(f"{base}.{e.name}")
        for desc in fp.message_type:
            walk(desc, base)

    return frozenset(names)


def build_model(
    proto_files,
    *,
    allow_oneof=False,
    skip_wkt=False,
    files_to_generate=(),
    skip_unsupported_fields=False,
):
    _to_gen = set(files_to_generate)
    supported_wkt_types = _supported_wkt_type_names(proto_files)
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
            # Only *unsupported* WKT types reject/drop now; supported
            # ones are kept in the model and resolve as ordinary
            # cross-file message refs.
            refs_wkt = (
                f.type in (_T.TYPE_MESSAGE, _T.TYPE_ENUM)
                and f.type_name.startswith(_WKT_TYPE_PREFIX)
                and f.type_name not in supported_wkt_types
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

            elif refs_wkt and skip_wkt:
                dropped.append(
                    DroppedField(
                        name=f.name,
                        full_name=f"{full}.{f.name}",
                        reason=f"references well-known type {f.type_name}",
                    )
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
        # Skip only *unsupported* imported WKT files wholesale. Supported WKT
        # files are walked so their messages enter the symbol table (fields
        # referencing them resolve as cross-file refs). Never skip a file we
        # were asked to generate (a user proto may live under google/protobuf/).
        if (
            fp.name.startswith(_WKT_DEP_PREFIX)
            and fp.name not in _WKT_SUPPORTED_FILES
            and fp.name not in _to_gen
        ):
            continue

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


def _message_field_targets(message):
    """Yield (field_name, target_full_name) for each message-typed field."""
    for f in message.fields:
        if f.type == _T.TYPE_MESSAGE:
            yield f.name, f.type_name


def find_back_edges(model):
    """Edges (message_full_name, field_name) that close a cycle.

    DFS in declaration order; an edge to a grey (on-stack) node is a
    back-edge. Forward/cross edges to finished nodes are excluded.
    """
    by_decl = sorted(model.messages, key=lambda m: m.decl_index)
    color = {}  # full_name -> "grey" | "black"
    back = set()

    def visit(name):
        color[name] = "grey"
        msg = model.message(name)
        for field_name, target in _message_field_targets(msg):
            if target not in color:
                visit(target)
            elif color[target] == "grey":
                back.add((name, field_name))

        color[name] = "black"

    for m in by_decl:
        if m.full_name not in color:
            visit(m.full_name)

    return frozenset(back)


def emission_order(model, *, opaque_recursion, back=None):
    """Topological order of the message-reference DAG, closest to IDL order.

    Among all valid topological orders we want the one that tracks proto
    declaration order wherever the dependency graph allows it -- i.e. the
    *lexicographically smallest* topological order keyed on declaration index.

    Cycles are rejected unless `opaque_recursion` cuts their back-edges first.
    `back` may be supplied by a caller that already ran the DFS (plan_emission)
    to avoid a second pass; it is computed here when omitted.
    """
    if back is None:
        back = find_back_edges(model)

    cut = back
    if cut and not opaque_recursion:
        m, f = sorted(cut)[0]
        raise GenError(
            f"{m.lstrip('.')}.{f} forms a cycle; recursive schemas are unsupported. "
            f"Use --ppb_opt=opaque_cycles to emit it as an opaque byte span."
        )

    decl = {m.full_name: m.decl_index for m in model.messages}

    # An edge u -> target ("u depends on target") means target must precede u.
    # Only message-typed fields create edges: a nested enum depends on nothing
    # and is emitted ahead of every schema alias (see emit_file).
    pending = {m.full_name: 0 for m in model.messages}
    dependents = {m.full_name: [] for m in model.messages}
    for m in model.messages:
        for field_name, target in _message_field_targets(m):
            if (m.full_name, field_name) in cut:
                continue

            pending[m.full_name] += 1
            dependents[target].append(m.full_name)

    heap = [(decl[n], n) for n, count in pending.items() if count == 0]
    heapq.heapify(heap)
    order = []
    while heap:
        _, n = heapq.heappop(heap)
        order.append(n)
        for dependent in dependents[n]:
            pending[dependent] -= 1
            if pending[dependent] == 0:
                heapq.heappush(heap, (decl[dependent], dependent))

    if len(order) != len(model.messages):
        raise GenError("internal: residual cycle after cut")

    return tuple(model.message(n) for n in order)


def compute_depths(model, *, opaque_fields):
    """max_depth per message over the cut graph (opaque fields don't recurse)."""
    memo = {}

    def depth(name):
        if name in memo:
            return memo[name]

        memo[name] = 0  # break any residual self-reference safely
        best = 0
        for field_name, target in _message_field_targets(model.message(name)):
            if (name, field_name) in opaque_fields:
                continue

            best = max(best, 1 + depth(target))

        memo[name] = best
        return best

    return {m.full_name: depth(m.full_name) for m in model.messages}


def _field_handler_hint(field, *, f_name, symbols, opaque, syntax, detect_unknown):
    """Return the ppb reader handler-factory call that matches this field.

    Used as a `// hint` line comment before the first descriptor for each
    field in an auto_schema<...> alias.
    """
    key = f"{f_name}::{cpp_ident(field.name)}"
    repeated = field.label == _T.LABEL_REPEATED

    if field.type == _T.TYPE_MESSAGE:
        if opaque:
            # Opaque back-edge: the descriptor is bytes<>, but refer to the cyclic
            # target's schema so users know to decode the span recursively.
            if repeated:
                inner = symbols.message_schema(field.type_name)
            else:
                inner = symbols.message_merge_schema(field.type_name)
        else:
            # Mirror the exact inner alias the descriptor uses: merge_schema for
            # singular message fields, schema for repeated.
            inner = _message_inner_alias(
                field,
                symbols,
                detect_unknown=detect_unknown,
                repeated=repeated,
            )
        return f"ppb::on_submessage<{key}, {inner}>(...)"

    if repeated:
        if field.type in _SCALAR_BASE or field.type == _T.TYPE_ENUM:
            if _field_is_packed(field, syntax):
                return f"ppb::on_bulk<{key}>(range_fn, elem_fn)"
        return f"ppb::on_each<{key}>(...)"

    return f"ppb::on<{key}>(...)"


def emit_enum(enum):
    short_name = cpp_ident(enum.full_name.rsplit(".", 1)[1])
    lines = [f"    enum class {short_name} : ::std::int32_t", "    {"]
    for v in enum.values:
        lines.append(f"        {cpp_ident(v.name)} = {v.number},")

    lines.append("    };")
    return "\n".join(lines)


def emit_file_enum(enum):
    """Emit a file-scope (top-level) enum wrapped in its package namespace."""
    parent = enum.full_name.rsplit(".", 1)[0]  # ".demo.Color" -> ".demo"
    namespace = _cpp_ns(parent)
    body = emit_enum(enum)
    return f"namespace {namespace}\n{{\n{body}\n}}\n"


def emit_nested_enums(message):
    """Emit a message's nested enum definitions in its own namespace block.

    A nested enum depends on nothing, so these are emitted ahead of every schema
    alias (see emit_file); that lets any message name another message's nested
    enum regardless of where the two schemas land in emission order.  Returns ""
    for a message with no nested enums.
    """
    if not message.enums:
        return ""

    body = "\n\n".join(emit_enum(e) for e in message.enums)
    return f"namespace {message.namespace}\n{{\n{body}\n}}\n"


def emit_message(
    model,
    message,
    *,
    strict_repeated_encoding,
    detect_unknown,
    opaque_fields,
    depths=None,
):
    """Emit the C++ namespace block for one message.

    Emits: field-key enum F (IDL order), max_depth constant, and the
    auto_schema alias.  Nested enum *definitions* are emitted separately and
    earlier by emit_nested_enums.
    """
    if depths is None:
        depths = compute_depths(model, opaque_fields=opaque_fields)

    ids = message.identifiers
    out = []
    out.append(f"namespace {message.namespace}")
    out.append("{")

    # Field-key enum (IDL order); auto_schema re-sorts internally.
    out.append(f"    enum class {ids.f} : ::std::int32_t")
    out.append("    {")
    for f in message.fields:
        out.append(f"        {cpp_ident(f.name)} = {f.number},")

    out.append("    };")
    out.append("")

    out.append(f"    constexpr ::std::size_t {ids.max_depth} = {depths[message.full_name]};")
    out.append("")

    # Descriptor list (IDL order); auto_schema re-sorts.  Each field is
    # prefixed with a hint comment naming its handler factory.
    items = []
    for f in message.fields:
        opaque = (message.full_name, f.name) in opaque_fields
        field_descs = map_field(
            f,
            syntax=message.syntax,
            strict_repeated_encoding=strict_repeated_encoding,
            f_name=ids.f,
            symbols=model.symbols,
            opaque=opaque,
            detect_unknown=detect_unknown,
        )
        hint = _field_handler_hint(
            f,
            f_name=ids.f,
            symbols=model.symbols,
            opaque=opaque,
            syntax=message.syntax,
            detect_unknown=detect_unknown,
        )
        items.append(f"// {hint}\n        {field_descs[0]}")
        items.extend(field_descs[1:])

    if detect_unknown or not message.fields:
        items.append("// ppb::on_unknown<>(...)\n        ::ppb::detect_unknown_fields<>")

    if not message.fields:
        full_name = message.full_name.lstrip(".")
        short_name = full_name.rsplit(".", 1)[-1]
        out.append(f"""\
    /*
     * {full_name} declares no fields, so this schema registers only the
     * unknown-field catch-all; a ppb::schema must hold at least one descriptor.
     *
     * If you only need to know whether {short_name} was present, do not pay to
     * handle unknown fields here.  In the CONTAINING message, register
     * ppb::on<F::that_field>(...) to receive this submessage's raw span: that is
     * presence/absence detection without descending into it.  Handle unknown
     * fields here only when you must inspect the (otherwise unknown) contents.
     */""")

    joined = ",\n        ".join(items)
    out.append(f"    using {ids.schema} = ::ppb::auto_schema<\n        {joined}>;")

    # merge_schema uses non-proto3_ (last_write_wins) variants for the
    # message's scalars, and references inner merge_schema for nested
    # message fields, so absent fields aren't dispatched when merging
    # into an already-populated child.
    merge_items = []
    for f in message.fields:
        opaque = (message.full_name, f.name) in opaque_fields
        merge_descs = map_field(
            f,
            syntax=message.syntax,
            strict_repeated_encoding=strict_repeated_encoding,
            f_name=ids.f,
            symbols=model.symbols,
            opaque=opaque,
            merge=True,
            detect_unknown=detect_unknown,
        )
        hint = _field_handler_hint(
            f,
            f_name=ids.f,
            symbols=model.symbols,
            opaque=opaque,
            syntax=message.syntax,
            detect_unknown=detect_unknown,
        )
        merge_items.append(f"// {hint}\n        {merge_descs[0]}")
        merge_items.extend(merge_descs[1:])

    if detect_unknown or not message.fields:
        merge_items.append("// ppb::on_unknown<>(...)\n        ::ppb::detect_unknown_fields<>")

    out.append("")
    if merge_items == items:
        out.append(f"    using {ids.merge_schema} = {ids.schema};")
    else:
        merge_joined = ",\n        ".join(merge_items)
        out.append(f"    using {ids.merge_schema} = ::ppb::auto_schema<\n        {merge_joined}>;")

    out.append("}")
    out.append("")
    return "\n".join(out)


def _header_name(proto_file_name):
    # "sub/a.proto" -> "sub/a.ppb.hpp"
    if not proto_file_name.endswith(".proto"):
        raise GenError(f"expected a .proto file name, got {proto_file_name!r}")

    return proto_file_name.removesuffix(".proto") + ".ppb.hpp"


@dataclasses.dataclass(frozen=True)
class EmissionPlan:
    """Whole-closure emission data shared by every file in a request.

    `order`, `opaque_fields`, and `depths` depend only on the model (derived from
    the descriptors) and the opaque_recursion flag, so they are identical for every
    generated file.  generate() computes one plan and threads it through, rather
    than rebuilding the graph once per output header.
    """

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


def emit_file(
    model,
    file_proto,
    *,
    strict_repeated_encoding,
    detect_unknown,
    plan,
    skip_wkt=False,
    files_to_generate=(),
):
    own = {m.full_name for m in model.messages if m.source_file == file_proto.name}

    out = ["#pragma once", "", "#include <ppb/ppb.hpp>", "", "// clang-format off"]
    to_gen = set(files_to_generate)
    wkt_redirected = False
    for dep in file_proto.dependency:
        if dep.startswith(_WKT_DEP_PREFIX) and dep not in to_gen:
            if dep in _WKT_SUPPORTED_FILES:
                if not wkt_redirected:
                    out.append("#include <ppb/wkt.ppb.hpp>")
                    wkt_redirected = True

                continue

            if skip_wkt:
                continue

            raise GenError(
                f"{file_proto.name!r} imports well-known type {dep!r}, which "
                f"protoc-gen-ppb does not support; pass "
                f"--ppb_opt=drop_foreign_type_fields to drop fields that "
                f"reference it (those fields will not be decoded)"
            )

        out.append(f'#include "{_header_name(dep)}"')

    out.append("")

    # Unknown-field detection retains unknown fields inside a supported WKT
    # scope, but the schemas in <ppb/wkt.ppb.hpp> lack unknown-field
    # catch-all.  For each WKT-with-fields type referenced in this file, splice
    # the catch-all on here once via a named alias; the message<> descriptors
    # reference it so the on_unknown handlers match.
    if detect_unknown:
        seen_wkt = set()
        for message in model.messages:
            if message.source_file != file_proto.name:
                continue

            for f in message.fields:
                if (
                    f.type == _T.TYPE_MESSAGE
                    and f.type_name not in seen_wkt
                    and _is_supported_wkt_with_fields(model.symbols, f.type_name)
                ):
                    seen_wkt.add(f.type_name)
                    wkt = model.symbols.message(f.type_name)
                    ids = wkt.identifiers
                    out.append(f"namespace {wkt.namespace}")
                    out.append("{")
                    out.append(
                        f"    using {ids.schema}_with_unknowns = "
                        f"::ppb::auto_schema<{ids.schema}, ::ppb::detect_unknown_fields<>>;"
                    )
                    out.append(
                        f"    using {ids.merge_schema}_with_unknowns = "
                        f"::ppb::auto_schema<{ids.merge_schema}, ::ppb::detect_unknown_fields<>>;"
                    )
                    out.append("}")
                    out.append("")

    dropped = [
        df
        for message in model.messages
        if message.source_file == file_proto.name
        for df in message.dropped_fields
    ]
    ignored_ranges = [
        (message.full_name, lo, hi)
        for message in model.messages
        if message.source_file == file_proto.name
        for (lo, hi) in message.ignored_extension_ranges
    ]
    ignored_defs = [
        full_name
        for message in model.messages
        if message.source_file == file_proto.name
        for full_name in message.ignored_extension_defs
    ]
    if dropped or ignored_ranges or ignored_defs:
        out.append("// Fields dropped / constructs ignored by protoc-gen-ppb")
        out.append("// (not decoded; these fields/constructs are dropped from the schema):")
        for df in dropped:
            out.append(f"//   ppb-dropped: {df.full_name} ({df.reason})")
        for owner, lo, hi in ignored_ranges:
            out.append(f"//   ppb-extension-range-ignored: {lo}-{hi} (in {owner})")
        for full_name in ignored_defs:
            out.append(f"//   ppb-extension-def-ignored: {full_name}")
        out.append("")

    # File-scope enums first: messages in this file may reference them.
    for enum in model.file_enums:
        if enum.source_file == file_proto.name:
            out.append(emit_file_enum(enum))

    # Nested enum definitions next, all ahead of any schema alias. A nested enum
    # depends on nothing, so emitting them up front (in declaration order) lets a
    # message name another message's nested enum no matter where the two schemas
    # land in emission order.
    for message in model.messages:
        if message.full_name in own and message.enums:
            out.append(emit_nested_enums(message))

    for message in plan.order:
        if message.full_name in own:
            out.append(
                emit_message(
                    model,
                    message,
                    strict_repeated_encoding=strict_repeated_encoding,
                    detect_unknown=detect_unknown,
                    opaque_fields=plan.opaque_fields,
                    depths=plan.depths,
                )
            )

    return "\n".join(out) + "\n"


def mangling_warnings(model):
    """Return a tuple of warning strings for every identifier the generator had to mangle."""
    out = []
    seen_segments = set()

    def note(context, name):
        if cpp_ident(name) != name:
            out.append(
                f"{PLUGIN_NAME}: warning: {context}: C++ keyword {name!r} "
                f"emitted as {cpp_ident(name)!r}"
            )

    def note_namespace(full_name):
        # Every component of a proto qualified name becomes a C++
        # namespace segment.  Warn on each distinct segment once, in
        # first-seen order for determinism.
        prefix = ""
        for part in full_name.strip(".").split("."):
            prefix = f"{prefix}.{part}"
            if prefix not in seen_segments:
                seen_segments.add(prefix)
                note(prefix, part)

    for m in model.messages:
        note_namespace(m.full_name)

    for e in model.file_enums:
        note_namespace(e.full_name)

    for m in model.messages:
        for e in m.enums:
            note_namespace(e.full_name)

    for m in model.messages:
        for f in m.fields:
            note(f"{m.full_name}.{f.name}", f.name)
        for e in m.enums:
            for v in e.values:
                note(f"{e.full_name}.{v.name}", v.name)

    for e in model.file_enums:
        for v in e.values:
            note(f"{e.full_name}.{v.name}", v.name)

    return tuple(out)


def empty_message_warnings(model):
    """Return a tuple of warning strings, one per zero-field message."""
    out = []
    for m in model.messages:
        if not m.fields:
            out.append(
                f"{PLUGIN_NAME}: warning: {m.full_name}: message has no fields; "
                f"registering the unknown-field catch-all only (read via on_unknown<>)"
            )

    return tuple(out)


def drop_warnings(model):
    """Return human-readable warnings for dropped fields, extensions, and definitions.

    Protoc surfaces them on stderr at generation time. The same drops are also
    recorded as sidecar comments in the schema header.
    """
    out = []
    for message in model.messages:
        for df in message.dropped_fields:
            out.append(f"{PLUGIN_NAME}: ppb-dropped: {df.full_name} ({df.reason})")

        for lo, hi in message.ignored_extension_ranges:
            out.append(
                f"{PLUGIN_NAME}: ppb-extension-range-ignored: {lo}-{hi} (in {message.full_name})"
            )

        for full_name in message.ignored_extension_defs:
            out.append(f"{PLUGIN_NAME}: ppb-extension-def-ignored: {full_name}")

    return tuple(out)


def required_downgrade_warnings(model):
    """Return a warning for each proto2 `required` field.

    PPB treats it as optional and does not enforce required-presence.
    """
    out = []
    for m in model.messages:
        if m.syntax != "proto2":
            continue

        for f in m.fields:
            if f.label == _T.LABEL_REQUIRED:
                out.append(
                    f"{PLUGIN_NAME}: warning: {m.full_name}.{f.name}: "
                    f"proto2 required field downgraded to optional; "
                    f"required-presence is not enforced"
                )

    return tuple(out)


def oneof_as_optional_warnings(model):
    """Return a warning for each real oneof expanded under oneof_as_optional.

    The schema models each member independently, so oneof exclusivity is not
    enforced.
    """
    out = []
    for m in model.messages:
        for name in m.real_oneof_names:
            out.append(
                f"{PLUGIN_NAME}: warning: {m.full_name}: "
                f"oneof {name!r} expanded as optional fields; "
                f"oneof exclusivity is not modelled"
            )

    return tuple(out)


@dataclasses.dataclass(frozen=True)
class Options:
    strict_repeated_encoding: bool
    detect_unknown: bool
    opaque_cycles: bool
    oneof_as_optional: bool
    drop_foreign_type_fields: bool
    drop_group_extension_fields: bool


def parse_options(parameter):
    strict_repeated_encoding = True  # default lean
    detect_unknown = False
    opaque_cycles = False
    oneof_as_optional = False
    drop_foreign_type_fields = False
    drop_group_extension_fields = False
    for raw in parameter.split(","):
        opt = raw.strip()
        if not opt:
            continue

        if opt.startswith("mode="):
            value = opt[len("mode=") :]
            if value not in ("none", "lean", "full"):
                raise GenError(f"unknown mode {value!r}")

            if value == "lean":
                strict_repeated_encoding = True
                detect_unknown = False
            elif value == "none":
                strict_repeated_encoding = False
                detect_unknown = False
            elif value == "full":
                strict_repeated_encoding = False
                detect_unknown = True
        elif opt == "detect_unknown":
            detect_unknown = True
        elif opt == "opaque_cycles":
            opaque_cycles = True
        elif opt == "oneof_as_optional":
            oneof_as_optional = True
        elif opt == "drop_foreign_type_fields":
            drop_foreign_type_fields = True
        elif opt == "drop_group_extension_fields":
            drop_group_extension_fields = True
        elif opt == "strict_repeated_encoding":
            strict_repeated_encoding = True
        else:
            raise GenError(f"unknown option {opt!r}")

    return Options(
        strict_repeated_encoding=strict_repeated_encoding,
        detect_unknown=detect_unknown,
        opaque_cycles=opaque_cycles,
        oneof_as_optional=oneof_as_optional,
        drop_foreign_type_fields=drop_foreign_type_fields,
        drop_group_extension_fields=drop_group_extension_fields,
    )


def generate(request):
    response = plugin_pb2.CodeGeneratorResponse()
    response.supported_features = plugin_pb2.CodeGeneratorResponse.FEATURE_PROTO3_OPTIONAL

    try:
        opts = parse_options(request.parameter)
        detect = opts.detect_unknown
        model = build_model(
            list(request.proto_file),
            allow_oneof=opts.oneof_as_optional,
            skip_wkt=opts.drop_foreign_type_fields,
            files_to_generate=request.file_to_generate,
            skip_unsupported_fields=opts.drop_group_extension_fields,
        )
        for w in mangling_warnings(model):
            sys.stderr.write(w + "\n")

        for w in empty_message_warnings(model):
            sys.stderr.write(w + "\n")

        for w in drop_warnings(model):
            sys.stderr.write(w + "\n")

        for w in required_downgrade_warnings(model):
            sys.stderr.write(w + "\n")

        for w in oneof_as_optional_warnings(model):
            sys.stderr.write(w + "\n")

        plan = plan_emission(model, opaque_recursion=opts.opaque_cycles)
        by_name = {fp.name: fp for fp in request.proto_file}
        for name in request.file_to_generate:
            fp = by_name[name]
            content = emit_file(
                model,
                fp,
                strict_repeated_encoding=opts.strict_repeated_encoding,
                detect_unknown=detect,
                plan=plan,
                skip_wkt=opts.drop_foreign_type_fields,
                files_to_generate=request.file_to_generate,
            )
            out = response.file.add()
            out.name = _header_name(name)
            out.content = content
    except GenError as exc:
        response.ClearField("file")
        response.error = str(exc)
    except Exception as exc:
        # protoc would otherwise surface a bare "plugin crashed" with a Python
        # traceback on its stderr; a structured response.error is friendlier.
        response.ClearField("file")
        response.error = f"internal error: {exc}"

    return response


def respond(data: bytes) -> plugin_pb2.CodeGeneratorResponse:
    try:
        request = plugin_pb2.CodeGeneratorRequest.FromString(data)
    except Exception as exc:
        # Catch-all: a structured error beats a traceback on protoc's
        # stderr.
        response = plugin_pb2.CodeGeneratorResponse()
        response.error = f"cannot decode CodeGeneratorRequest: {exc}"
        return response

    return generate(request)


def _request_from_descriptor_set(path, files_to_generate, parameter):
    """Build a CodeGeneratorRequest from a serialized FileDescriptorSet."""
    with open(path, "rb") as fh:
        fds = descriptor_pb2.FileDescriptorSet.FromString(fh.read())

    request = plugin_pb2.CodeGeneratorRequest()
    request.proto_file.extend(fds.file)
    request.file_to_generate.extend(files_to_generate)
    request.parameter = parameter
    return request


def _wkt_file_descriptor_proto(proto_name):
    """Return the FileDescriptorProto for a supported WKT, from the Python runtime."""
    import importlib

    mod = importlib.import_module(_WKT_PB2_MODULES[proto_name])
    fdp = descriptor_pb2.FileDescriptorProto()
    mod.DESCRIPTOR.CopyToProto(fdp)
    return fdp


def _inject_supported_wkt(request):
    """Add any supported WKT FileDescriptorProto missing from the closure.

    A copy already in the request wins (matches protoc's view).
    """
    present = {fp.name for fp in request.proto_file}
    for proto_name in _WKT_SUPPORTED_FILES:
        if proto_name not in present:
            request.proto_file.append(_wkt_file_descriptor_proto(proto_name))


def _missing_non_wkt_dependency(request):
    """Find the first dependency referenced by the closure that is absent from it.

    Returns None when the closure is self-contained. The missing dependency
    must not be a supported WKT (which is injected separately). Protoc's
    --include_imports normally guarantees a self-contained closure.
    """
    present = {fp.name for fp in request.proto_file}
    for fp in request.proto_file:
        for dep in fp.dependency:
            if dep not in present and dep not in _WKT_SUPPORTED_FILES:
                return dep

    return None


def _write_response(response, out_dir):
    """Write each response.file under out_dir (creating subdirs); 1 on error."""
    if response.error:
        sys.stderr.write(f"{PLUGIN_NAME}: {response.error}\n")
        return 1

    for f in response.file:
        dest = os.path.join(out_dir, f.name)
        os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
        with open(dest, "w") as fh:
            fh.write(f.content)

    return 0


def _strip_generated_prologue(content):
    """Drop a generated header's leading prologue and return the body.

    The fused bundle emits one shared prologue instead of the per-header one.
    """
    lines = content.split("\n")
    skip = {"#pragma once", "#include <ppb/ppb.hpp>", "// clang-format off", ""}
    i = 0
    while i < len(lines) and lines[i].strip() in skip:
        i += 1

    return "\n".join(lines[i:])


def _emit_wkt_bundle(path):
    import google.protobuf

    request = plugin_pb2.CodeGeneratorRequest()
    request.parameter = ""
    for proto_name in _WKT_BUNDLE_ORDER:
        request.proto_file.append(_wkt_file_descriptor_proto(proto_name))
        request.file_to_generate.append(proto_name)

    response = generate(request)
    if response.error:
        sys.stderr.write(f"{PLUGIN_NAME}: {response.error}\n")
        return 1

    by_name = {f.name: f.content for f in response.file}
    bodies = [_strip_generated_prologue(by_name[_header_name(p)]) for p in _WKT_BUNDLE_ORDER]
    # pyrefly: ignore[missing-attribute]  # absent from the stubs, present at runtime
    banner = f"""\
// Generated by protoc-gen-ppb --emit-wkt-bundle. DO NOT EDIT.
// protobuf runtime: {google.protobuf.__version__}
//
// Fused schemas for the supported well-known types. none, lean, and full
// are identical for these six files (the only repeated field,
// FieldMask.paths, is a never-packed string), so one header serves all modes.
"""
    fused = (
        "#pragma once\n\n"
        "#include <ppb/ppb.hpp>\n\n" + banner + "\n// clang-format off\n" + "".join(bodies)
    )
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w") as fh:
        fh.write(fused)

    return 0


def _run_standalone(argv):
    parser = argparse.ArgumentParser(prog=PLUGIN_NAME)
    parser.add_argument("--descriptor-set")
    parser.add_argument("--opt", default="")
    parser.add_argument("--out", default=".")
    parser.add_argument("--emit-wkt-bundle")
    parser.add_argument("files", nargs="*")
    args = parser.parse_args(argv)

    if args.emit_wkt_bundle is not None:
        return _emit_wkt_bundle(args.emit_wkt_bundle)

    if not args.descriptor_set:
        parser.error("--descriptor-set is required (or use --emit-wkt-bundle)")

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
