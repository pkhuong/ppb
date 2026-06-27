import subprocess
import sys
from pathlib import Path

from google.protobuf import descriptor_pb2 as d

from tests.conftest import make_file_proto

HERE = Path(__file__).resolve().parent
PLUGIN = HERE.parent / "protoc_gen_ppb.py"
T = d.FieldDescriptorProto


def _descriptor_set(*file_protos):
    fds = d.FileDescriptorSet()
    fds.file.extend(file_protos)
    return fds.SerializeToString()


def _scalars_fp():
    m = d.DescriptorProto(name="S")
    m.field.add(name="x", number=1, type=T.TYPE_INT32, label=T.LABEL_OPTIONAL)
    return make_file_proto(m, name="s.proto", package="demo")


def test_standalone_generates_header(tmp_path):
    ds = tmp_path / "set.pb"
    ds.write_bytes(_descriptor_set(_scalars_fp()))
    out = tmp_path / "out"
    res = subprocess.run(
        [sys.executable, str(PLUGIN), "--descriptor-set", str(ds), "--out", str(out), "s.proto"],
        capture_output=True,
        text=True,
        cwd=str(HERE.parent),
    )
    assert res.returncode == 0, res.stderr
    header = out / "s.ppb.hpp"
    assert header.exists()
    assert "namespace ppb_gen::demo" in header.read_text()


def test_standalone_matches_plugin_protocol(tmp_path):
    from google.protobuf.compiler import plugin_pb2

    req = plugin_pb2.CodeGeneratorRequest()
    req.file_to_generate.append("s.proto")
    req.proto_file.append(_scalars_fp())
    res = subprocess.run(
        [sys.executable, str(PLUGIN)],
        input=req.SerializeToString(),
        capture_output=True,
        cwd=str(HERE.parent),
    )
    assert res.returncode == 0, res.stderr
    resp = plugin_pb2.CodeGeneratorResponse.FromString(res.stdout)
    assert resp.file and resp.file[0].name == "s.ppb.hpp"


def test_emit_wkt_bundle_is_hermetic(tmp_path):
    bundle = tmp_path / "wkt.ppb.hpp"
    res = subprocess.run(
        [sys.executable, str(PLUGIN), "--emit-wkt-bundle", str(bundle)],
        capture_output=True,
        text=True,
        cwd=str(HERE.parent),
    )
    assert res.returncode == 0, res.stderr
    text = bundle.read_text()
    assert text.count("#pragma once") == 1
    assert text.count("#include <ppb/ppb.hpp>") == 1
    assert "// clang-format off" in text
    assert "namespace ppb_gen::google::protobuf" in text
    for name in ("Any", "Duration", "Empty", "FieldMask", "Timestamp", "BoolValue"):
        assert name in text
