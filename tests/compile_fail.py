#!/usr/bin/env python3
"""Drive negative compile tests for the C++ wrapper.

Usage: compile_fail.py <CXX> <CXXFLAGS> <source>
"""
import re
import shlex
import subprocess
import sys


def extract_cases(src):
    """Return [(macro, regex), ...] for every PPB_FAIL_* block."""
    cases = []
    regex = None
    with open(src) as f:
        for line in f:
            m = re.match(r"/\* expect-error: (.*?) \*/", line)
            if m:
                regex = m.group(1)
                continue
            m = re.match(r"#ifdef (PPB_FAIL_\S+)", line)
            if m:
                cases.append((m.group(1), regex))
                regex = None
    return cases


def main():
    if len(sys.argv) != 4:
        sys.exit(f"Usage: {sys.argv[0]} <CXX> <CXXFLAGS> <source>")

    cxx, flags, src = sys.argv[1], sys.argv[2], sys.argv[3]
    cxx_argv = shlex.split(cxx)
    flags_argv = shlex.split(flags)
    cases = extract_cases(src)

    for macro, regex in cases:
        print(f"[compile_fail] {macro} ... ", end="", flush=True)
        cmd = cxx_argv + flags_argv + [f"-D{macro}", "-c", src, "-o", "/dev/null"]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        out = proc.stderr
        if re.search(regex, out):
            print("ok")
        else:
            print(f"FAIL\n--- expected regex: {regex}\n--- got:\n{out}")
            sys.exit(1)

    # Sanity: with no macro, the file must compile cleanly.
    print("[compile_fail] (no macros) ... ", end="", flush=True)
    cmd = cxx_argv + flags_argv + ["-c", src, "-o", "/dev/null"]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode == 0:
        print("ok")
    else:
        print(f"FAIL\n--- got:\n{proc.stderr}")
        sys.exit(1)


if __name__ == "__main__":
    main()
