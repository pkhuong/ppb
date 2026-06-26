#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["protobuf>=4"]
# ///
"""protoc-gen-ppb: emit ppb::schema headers from a .proto descriptor closure."""

import dataclasses
import heapq
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
        items.append(field_descs[0])
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
        merge_items.append(merge_descs[0])
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
