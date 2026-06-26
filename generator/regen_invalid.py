#!/usr/bin/env -S uv run python
"""Regenerate committed expected-error / expected-stderr golden files for testdata/invalid/."""

import subprocess
import sys
import tempfile
from contextlib import redirect_stderr
from io import StringIO
from pathlib import Path

HERE = Path(__file__).resolve().parent

# Add the generator directory itself to the path so we can import protoc_gen_ppb.
sys.path.insert(0, str(HERE))

from google.protobuf import descriptor_pb2 as d  # noqa: E402
from google.protobuf.compiler import plugin_pb2 as p  # noqa: E402

import protoc_gen_ppb as gen  # noqa: E402

INVALID = HERE / "testdata" / "invalid"


def regen_one(proto_path):
    name = proto_path.name  # e.g. "cycle.proto"
    with tempfile.TemporaryDirectory() as tmp:
        fds_path = Path(tmp) / "fds.pb"
        result = subprocess.run(
            [
                "protoc",
                f"--proto_path={INVALID}",
                f"--descriptor_set_out={fds_path}",
                "--include_imports",
                name,
            ],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            print(f"  SKIP {name}: protoc failed: {result.stderr.strip()}", file=sys.stderr)
            return

        fds = d.FileDescriptorSet.FromString(fds_path.read_bytes())

    req = p.CodeGeneratorRequest()
    req.proto_file.extend(fds.file)
    req.file_to_generate.append(name)
    opt_path = proto_path.with_suffix(".opt")
    if opt_path.exists():
        req.parameter = opt_path.read_text().strip()

    buf = StringIO()
    with redirect_stderr(buf):
        resp = gen.generate(req)

    stderr_out = buf.getvalue()

    if resp.error:
        out_path = proto_path.with_suffix(".expected-error")
        out_path.write_text(resp.error + "\n")
        print(f"  wrote {out_path.name}")
        return out_path
    elif stderr_out:
        out_path = proto_path.with_suffix(".expected-stderr")
        out_path.write_text(stderr_out)
        print(f"  wrote {out_path.name}")
        return out_path
    else:
        print(f"  SKIP {name}: no error and no stderr warnings; nothing to golden")
        return None


def main():
    written = set()
    for proto in sorted(INVALID.glob("*.proto")):
        out_path = regen_one(proto)
        if out_path is not None:
            written.add(out_path)

    # Remove any golden whose proto no longer produces that outcome (or was
    # deleted): regen is the source of truth, so a leftover here is stale. The
    # toplevel `generator_test` diff then trips on the deletion until it's committed.
    existing = set(INVALID.glob("*.expected-error")) | set(INVALID.glob("*.expected-stderr"))
    for stale in sorted(existing - written):
        stale.unlink()
        print(f"  removed stale {stale.name}")

    print("done")


if __name__ == "__main__":
    main()
