#!/usr/bin/env python3
"""Source-level mutation testing for C files.

Systematically introduces small changes (mutants) to source files and
checks whether BUILD_CMD + TEST_CMD catch them.  A mutant that doesn't
build is a useless mutation; a mutant that passes TEST_CMD (a
"survivor") means the mutated code might be badly tested.

The source code can be annotated to opt specific lines out of
mutations.  The script returns with exit code is 1 if any unexpected
survivor is found, 0 otherwise.

Source annotations
------------------
  /* mutant-ok: substring */
    All mutation generators that contain `substring` are known
    equivalent mutants. Multiple per line OK.  Reported separately;
    does not affect exit code.

  /* mutant-skip */   (or  // mutant-skip)
    Disables ALL mutations on that line.

  /* mutant-triaged: <explanation> */   (or  // mutant-triaged: ...)
    All survivors on this line are known-hard-to-test debt.  The explanation
    is printed in the final report.  Does not affect exit code.

Structure
---------

1. parse_annotations / Annotations
   Scans the files for special comments that opt lines out of mutations,
   or marks specific mutations as expected to be undetected.

1. prepare_sources / SourceInfo
   Each file is read once, then `_blank_non_code` performs a bytewise
   replacement of comments, string/char literals, preprocessor
   directives, and assert() bodies with spaces — preserving offsets so
   regex matches on the mask apply directly to the original.
   mutant-skip lines are blanked here.

2. generate_mutations / Mutation
   Each generator walks the blanked code looking for its target
   pattern and emits Mutation objects.  Generators are independent;
   dedup_mutations merges identical (file, offset, original,
   replacement) entries afterward.

3. evaluate_mutants / EvalResults
   For each mutation: write mutant, run BUILD_CMD (stillborn?), run TEST_CMD,
   git checkout.  Survivors are classified as expected (mutant-ok), triaged
   (mutant-triaged), or unexpected (exit code 1).

Related work
------------

See https://testing.googleblog.com/2021/04/mutation-testing.html for
an overview of mutation testing.

This script implements an extremely low tech approach: build a
byte-for-byte shadow of each source file in which comments,
string/char literals, preprocessor directives, and assert() bodies are
blanked out with spaces of the same length.  Regex patterns are then
matched against the mask; because offsets are preserved, each match
can be applied directly to the original text without re-parsing.

The approach was inspired by C-Reduce
([Regehr et al., "Test-Case Reduction for C Compiler Bugs"](https://dl.acm.org/doi/10.1145/2345156.2254104),
https://github.com/csmith-project/creduce), with an extra blanking
step because the goal is to introduce minimal changes, while C-Reduce
wants to shrink a file as much as possible (it would be interesting to
look for the smallest file that still passes the test suite, but the
result might be hard to interpret).

Groce et al's [universalmutator](https://github.com/agroce/universalmutator)
([Groce et al., "An Extensible, Regular-Expression-Based Tool for Multi-Language Mutant Generation"](https://mir.cs.illinois.edu/marinov/publications/GroceETAL18UniversalMutator.pdf)) was initially a
pretty direct port of line-based the regex rewrite rule approach, but
without any blanking step.  It now supports [comby](https://github.com/comby-tools/comby)
based rewriting rules, to avoid generating too many invalid ("stillborn") mutants.

There are also smarter ways to do this.

* [SRCIROR](https://github.com/TestingResearchIllinois/srciror)
  ([Hariri and Shi, "SRCIROR: a toolset for mutation testing of C source code and LLVM intermediate representation"](https://dl.acm.org/doi/10.1145/3238147.3240482)) can generate mutation
  by walking the Clang AST, *or* by directly mutating LLVM IR.
  That's a lot more reliable and powerful than this hack, but
  also a lot more complex.

* [Mull](https://github.com/mull-project/mull)
  ([Denisov and Pankevich, "Mull It Over: Mutation Testing Based on LLVM"](https://lowlevelbits.org/pdfs/Mull_Mutation_2018.pdf))
  Mutates LLVM IR directly, with all the mutants compiled simultaneously
  and hidden behind feature flags.  This both avoids generating invalid
  source code, and means mutated binaries are only compiled once, rather
  than once per mutation.  That's pretty important for C++ projects.

The simple approach here (including hand-rolled LL parsers) can avoid
a lot of stillborn mutants, but is a lot simpler than SRCIROR or Mull.
The more complex approaches make a lot of sense for larger programs,
especially in C++ or Rust, where compilation and link times can be
painful.  That's not the case for ppb.
"""

import argparse
import bisect
from concurrent.futures import ThreadPoolExecutor
import os
import re
import tempfile
import shlex
import signal
import subprocess
import sys
import time
from dataclasses import dataclass, replace
from pathlib import Path

REPO_ROOT = Path(__file__).parent.resolve()
DEFAULT_FILES = ["src/*.c", "src/*.h"]  # glob patterns, relative to REPO_ROOT
TEST_TIMEOUT = 10  # seconds per mutant
BUILD_CMD = ["make", "all"]
TEST_CMD = ["make", "test"]
IFDEF_BLANK_IDS: set[str] = {"__FRAMAC__"}  # #ifdef <ID> ... #endif blocks are blanked


@dataclass
class Annotations:
    expected_survivors: set[tuple[Path, int, str, str]]
    expected_generators: dict[tuple[Path, int], list[str]]
    triaged_lines: dict[tuple[Path, int], str]


_MUTATION_ANNOTATION_RE = re.compile(r"mutant-ok:\s*'([^']+)'\s*->\s*'([^']+)'")
_MUTATOR_ANNOTATION_RE = re.compile(r"mutant-ok:\s*'([^']+)'(?!\s*->)")
_TRIAGED_RE = re.compile(r"mutant-triaged:\s*(.+?)(?=\s*\*/|\s*$)", re.MULTILINE)


def parse_annotations(sources: dict[Path, str]) -> Annotations:
    expected_survivors: set[tuple[Path, int, str, str]] = set()
    expected_generators: dict[tuple[Path, int], list[str]] = {}
    triaged_lines: dict[tuple[Path, int], str] = {}
    for path, source in sources.items():
        for line_no, line in enumerate(source.splitlines(), 1):
            for m in _MUTATION_ANNOTATION_RE.finditer(line):
                expected_survivors.add((path, line_no, m.group(1), m.group(2)))
            for m in _MUTATOR_ANNOTATION_RE.finditer(line):
                expected_generators.setdefault((path, line_no), []).append(m.group(1))
            m = _TRIAGED_RE.search(line)
            if m:
                triaged_lines[(path, line_no)] = m.group(1).strip()
    return Annotations(expected_survivors, expected_generators, triaged_lines)


def is_expected_survivor(m: "Mutation", ann: Annotations) -> bool:
    if (m.file, m.line_no, m.original, m.replacement) in ann.expected_survivors:
        return True
    patterns = ann.expected_generators.get((m.file, m.line_no), ())
    return any(pat in gen for pat in patterns for gen in m.generators)


@dataclass
class SourceInfo:
    path: Path
    source: str
    code: str
    line_starts: tuple[int, ...]


def _skip_quoted(source: str, n: int, start: int, quote: str) -> int:
    """Return index one past the closing quote matching source[start]."""
    j = start + 1
    while j < n:
        if source[j] == "\\":
            j += 2
        elif source[j] == quote:
            return j + 1
        else:
            j += 1
    return j


_IFDEF_BLANK_COND_DIRECTIVES = frozenset(
    ["if", "ifdef", "ifndef", "else", "elif", "elifdef", "elifndef"]
)
_IFDEF_BLANK_DIRECTIVE_RE = re.compile(r"#[ \t]*([a-z]+)")
_IFDEF_BLANK_IFDEF_RE = re.compile(r"#[ \t]*ifdef[ \t]+(\w+)")


def _scan_to_ifdef_blank_endif(source: str, start: int, trigger_id: str) -> int:
    """Scan from just after '#ifdef <trigger_id>' to its matching '#endif'; return position after it."""
    n = len(source)
    i = start

    while i < n:
        if source[i : i + 2] == "/*":
            j = source.find("*/", i + 2)
            i = j + 2 if j != -1 else n
            continue

        if source[i : i + 2] == "//":
            j = source.find("\n", i + 2)
            i = (j + 1) if j != -1 else n
            continue

        if source[i] in ('"', "'"):
            i = _skip_quoted(source, n, i, source[i])
            continue

        if source[i] == "#":
            line_start = source.rfind("\n", 0, i) + 1
            if source[line_start:i].strip() == "":
                j = i
                while j < n:
                    if source[j] == "\\" and j + 1 < n and source[j + 1] == "\n":
                        j += 2
                    elif source[j] == "\n":
                        break
                    else:
                        j += 1
                m = _IFDEF_BLANK_DIRECTIVE_RE.match(source, i)
                if m:
                    kw = m.group(1)
                    if kw in _IFDEF_BLANK_COND_DIRECTIVES:
                        raise ValueError(
                            f"nested #{kw} inside #ifdef {trigger_id} block"
                        )
                    if kw == "endif":
                        return j + 1 if j < n else n
                i = j
                continue

        i += 1

    raise ValueError(f"unterminated #ifdef {trigger_id} block (no matching #endif)")


def _blank_non_code(source: str) -> str:
    """Return a blanked out code-only string with non-code regions
    (comments, strings, preprocessor, assert bodies, mutant-skip
    lines) replaced by spaces.
    """
    result = list(source)
    n = len(source)
    i = 0

    def blank(start: int, end: int) -> None:
        result[start:end] = ["\n" if c == "\n" else " " for c in result[start:end]]

    def find_assertion_length(pos: int) -> int:
        for kw in ("static_assert", "assert"):
            klen = len(kw)
            if source[pos : pos + klen] != kw:
                continue
            prev_ch = source[pos - 1] if pos > 0 else " "
            next_ch = source[pos + klen] if pos + klen < n else " "
            if not (prev_ch.isalnum() or prev_ch == "_") and not (
                next_ch.isalnum() or next_ch == "_"
            ):
                return klen
        return 0

    while i < n:
        if source[i : i + 2] == "/*":
            j = source.find("*/", i + 2)
            end = (j + 2) if j != -1 else n
            if "mutant-skip" in source[i + 2 : end - 2]:
                line_start = source.rfind("\n", 0, i) + 1
                line_end = source.find("\n", end)
                if line_end == -1:
                    line_end = n
                blank(line_start, line_end)
                i = line_end
            else:
                blank(i, end)
                i = end
            continue

        if source[i : i + 2] == "//":
            j = source.find("\n", i + 2)
            end = j if j != -1 else n
            if "mutant-skip" in source[i + 2 : end]:
                line_start = source.rfind("\n", 0, i) + 1
                blank(line_start, end)
            else:
                blank(i, end)
            i = end
            continue

        # Preprocessor directive — a # that is the first non-whitespace on
        # its logical line (honouring backslash continuations).
        if source[i] == "#":
            line_start = source.rfind("\n", 0, i) + 1
            if source[line_start:i].strip() == "":
                j = i
                while j < n:
                    if source[j] == "\\" and j + 1 < n and source[j + 1] == "\n":
                        j += 2  # line continuation
                    elif source[j] == "\n":
                        break
                    else:
                        j += 1
                m = _IFDEF_BLANK_IFDEF_RE.match(source, i)
                if m and m.group(1) in IFDEF_BLANK_IDS:
                    block_end = _scan_to_ifdef_blank_endif(
                        source, j + 1 if j < n else n, m.group(1)
                    )
                    blank(line_start, block_end)
                    i = block_end
                else:
                    blank(i, j)
                    i = j
                continue

        if source[i] in ('"', "'"):
            j = _skip_quoted(source, n, i, source[i])
            blank(i, j)
            i = j
            continue

        # assert() and static_assert() — blank the entire call so its
        # condition is not treated as production logic to mutate.
        klen = find_assertion_length(i)
        if klen:
            j = i + klen
            while j < n and source[j] in " \t":
                j += 1
            if j < n and source[j] == "(":
                depth = 1
                j += 1
                while j < n and depth > 0:
                    if source[j] in ('"', "'"):
                        j = _skip_quoted(source, n, j, source[j])
                    elif source[j] == "(":
                        depth += 1
                        j += 1
                    elif source[j] == ")":
                        depth -= 1
                        j += 1
                    else:
                        j += 1
                blank(i, j)
                i = j
                continue

        i += 1

    return "".join(result)


def _build_line_index(source: str) -> tuple[int, ...]:
    starts = [0]
    for idx, ch in enumerate(source):
        if ch == "\n":
            starts.append(idx + 1)
    return tuple(starts)


def prepare_sources(sources: dict[Path, str]) -> list[SourceInfo]:
    return [
        SourceInfo(
            path=path,
            source=source,
            code=_blank_non_code(source),
            line_starts=_build_line_index(source),
        )
        for path, source in sources.items()
    ]


@dataclass
class Mutation:
    file: Path
    offset: int  # byte offset in the file
    original: str  # exact text being replaced
    replacement: str  # text to substitute
    line_no: int  # 1-based (display only)
    col: int  # 0-based (display only)
    generators: tuple[str, ...]  # which mutation generator(s) produced this

    def __lt__(self, other: "Mutation") -> bool:
        return (self.file, self.line_no, self.col) < (
            other.file,
            other.line_no,
            other.col,
        )

    def label(self) -> str:
        rel = self.file.relative_to(REPO_ROOT)
        gens = "[" + ", ".join(self.generators) + "]"
        orig = self.original if len(self.original) <= 40 else self.original[:37] + "..."
        return (
            f"{rel}:{self.line_no}:{self.col}  {gens}  {orig!r} -> {self.replacement!r}"
        )


def apply_mutation(source: str, m: Mutation) -> str:
    assert source[m.offset : m.offset + len(m.original)] == m.original, (
        f"Mutation offset mismatch at {m.label()}: "
        f"expected {m.original!r}, got {source[m.offset:m.offset+len(m.original)]!r}"
    )
    return source[: m.offset] + m.replacement + source[m.offset + len(m.original) :]


def _dedup_mutations(mutations: list[Mutation]) -> list[Mutation]:
    seen: dict[tuple, Mutation] = {}
    for m in mutations:
        key = (m.file, m.offset, m.original, m.replacement)
        if key in seen:
            seen[key] = replace(
                seen[key], generators=seen[key].generators + m.generators
            )
        else:
            seen[key] = m
    return list(seen.values())


def _find_matching_paren(text: str, open_pos: int) -> int | None:
    """Return index one past the closing ')' matching the '(' at open_pos, or None."""
    depth = 1
    j = open_pos + 1
    n = len(text)
    while j < n and depth > 0:
        if text[j] == "(":
            depth += 1
        elif text[j] == ")":
            depth -= 1
        j += 1
    return j if depth == 0 else None


def _offset_to_linecol(offset: int, line_starts: tuple[int, ...]) -> tuple[int, int]:
    line = bisect.bisect_right(line_starts, offset) - 1
    return line + 1, offset - line_starts[line]


def find_re_mutations(
    infos: list[SourceInfo], operators: list[tuple[str, str, str]]
) -> list[Mutation]:
    mutations: list[Mutation] = []

    for info in infos:
        for pattern, replacement, rule_name in operators:
            for m in re.finditer(pattern, info.code):
                # If the pattern has a capture group, group 1 is the actual
                # token to replace (lookarounds surround it).  Otherwise use
                # the entire match.
                if m.lastindex is not None and m.lastindex >= 1:
                    start, end = m.start(1), m.end(1)
                else:
                    start, end = m.start(0), m.end(0)

                line_no, col = _offset_to_linecol(start, info.line_starts)
                mutations.append(
                    Mutation(
                        file=info.path,
                        offset=start,
                        original=info.source[start:end],
                        replacement=replacement,
                        line_no=line_no,
                        col=col,
                        generators=(rule_name,),
                    )
                )

    return mutations


_STRUCTURAL_CHARS = frozenset("{}();, \t")


def find_line_deletions(infos: list[SourceInfo]) -> list[Mutation]:
    mutations: list[Mutation] = []

    for info in infos:
        for line_idx, line_start in enumerate(info.line_starts):
            newline_pos = info.source.find("\n", line_start)
            content_end = newline_pos if newline_pos != -1 else len(info.source)

            code_line = info.code[line_start:content_end]
            code_content = code_line.strip()
            if not code_content:
                continue
            if not code_line[0].isspace():  # not indented, probably a declaration
                continue
            if code_line.startswith("    return"):
                continue  # one-indent return, probably unconditional
            if all(c in _STRUCTURAL_CHARS for c in code_content):
                continue
            line_text = info.source[line_start:content_end]
            lstripped = line_text.lstrip()
            leading_len = len(line_text) - len(lstripped)
            content_start = line_start + leading_len
            content_stop = content_start + len(lstripped.rstrip())
            mutations.append(
                Mutation(
                    file=info.path,
                    offset=content_start,
                    original=info.source[content_start:content_stop],
                    replacement="",
                    line_no=line_idx + 1,
                    col=leading_len,
                    generators=("delete",),
                )
            )

    return mutations


_IF_RE = re.compile(r"\bif\s*\(")


def find_if_force_mutations(infos: list[SourceInfo]) -> list[Mutation]:
    mutations: list[Mutation] = []

    for info in infos:
        for m in _IF_RE.finditer(info.code):
            paren_start = m.end() - 1
            paren_end = _find_matching_paren(info.code, paren_start)
            if paren_end is None:
                continue

            condition = info.source[paren_start + 1 : paren_end - 1]
            original = info.source[paren_start:paren_end]
            line_no, col = _offset_to_linecol(paren_start, info.line_starts)

            for replacement, gen in (
                (f"(1 | !!({condition}))", "if:force_true"),
                (f"(0 & !!({condition}))", "if:force_false"),
            ):
                mutations.append(
                    Mutation(
                        file=info.path,
                        offset=paren_start,
                        original=original,
                        replacement=replacement,
                        line_no=line_no,
                        col=col,
                        generators=(gen,),
                    )
                )

            if_start = m.start()
            if_line_no, if_col = _offset_to_linecol(if_start, info.line_starts)
            mutations.append(
                Mutation(
                    file=info.path,
                    offset=if_start,
                    original=info.source[if_start:paren_end],
                    replacement="",
                    line_no=if_line_no,
                    col=if_col,
                    generators=("if:remove",),
                )
            )

    return mutations


_WHILE_RE = re.compile(r"\bwhile\s*\(")


def find_while_mutations(infos: list[SourceInfo]) -> list[Mutation]:
    mutations: list[Mutation] = []

    for info in infos:
        for m in _WHILE_RE.finditer(info.code):
            while_start = m.start()
            paren_start = m.end() - 1
            paren_end = _find_matching_paren(info.code, paren_start)
            if paren_end is None:
                continue

            condition = info.source[paren_start + 1 : paren_end - 1]
            line_no, col = _offset_to_linecol(while_start, info.line_starts)

            mutations.append(
                Mutation(
                    file=info.path,
                    offset=while_start,
                    original=info.source[while_start:paren_end],
                    replacement="",
                    line_no=line_no,
                    col=col,
                    generators=("while:remove",),
                )
            )

            mutations.append(
                Mutation(
                    file=info.path,
                    offset=paren_start,
                    original=info.source[paren_start:paren_end],
                    replacement=f"(0 & !!({condition}))",
                    line_no=line_no,
                    col=col,
                    generators=("while:force_false",),
                )
            )

    return mutations


def generate_mutations(
    infos: list[SourceInfo],
    operators: list[tuple[str, str, str]],
    delete: bool,
) -> list[Mutation]:
    mutations = find_re_mutations(infos, operators)
    mutations += find_if_force_mutations(infos)
    mutations += find_while_mutations(infos)
    if delete:
        mutations += find_line_deletions(infos)
    return _dedup_mutations(mutations)


@dataclass
class EvalResults:
    survived: tuple[Mutation, ...]
    triaged: tuple[Mutation, ...]
    expected_survived: tuple[Mutation, ...]
    counts: dict[str, int]
    triaged_lines: dict[tuple[Path, int], str]


def _run_cmd(cmd: list[str], *, inherit_streams: bool = False) -> str:
    proc = None
    streams = None if inherit_streams else subprocess.DEVNULL
    try:
        proc = subprocess.Popen(
            cmd,
            cwd=REPO_ROOT,
            stdout=streams,
            stderr=streams,
            start_new_session=True,  # XXX: this also means C-c won't propagate :\
        )
        proc.wait(timeout=TEST_TIMEOUT)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(proc.pid, signal.SIGTERM)
            time.sleep(TEST_TIMEOUT / 10)
        except ProcessLookupError:
            pass
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        proc = None
        return "timeout"
    finally:
        if proc is not None:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
    return "passed" if proc.returncode == 0 else "failed"


def _evaluate_one_mutant(
    sources: dict[Path, str],
    mutation: Mutation | None,
    *,
    inherit_streams: bool = False,
) -> str:
    try:
        if mutation is not None:
            mutant = apply_mutation(sources[mutation.file], mutation)
            mutation.file.write_text(mutant)
        build = _run_cmd(BUILD_CMD, inherit_streams=inherit_streams)
        if build != "passed":
            return "stillborn" if build == "failed" else "timeout"
        return _run_cmd(TEST_CMD, inherit_streams=inherit_streams)
    finally:
        if mutation is not None:
            subprocess.run(
                ["git", "checkout", "--", str(mutation.file)],
                cwd=REPO_ROOT,
                check=True,
                capture_output=True,
            )


def _evaluate_one_mutant_bwrap(
    sources: dict[Path, str],
    mutation: Mutation | None,
    bwrap: str = "bwrap",
    *,
    inherit_streams: bool = False,
) -> str:
    with tempfile.NamedTemporaryFile(mode="w") as tmp:
        if mutation is not None:
            mutant = apply_mutation(sources[mutation.file], mutation)
            tmp.write(mutant)
            tmp.flush()

        # Build and test in one shell command.
        # Exit codes: 2 = stillborn (build failed), 0 = passed, 1 = failed,
        # 124 = timeout (test timed out via the timeout(1) command).
        # The if/elif/else normalises any other non-zero test exit code to 1.
        shell_cmd = (
            shlex.join(BUILD_CMD)
            + " || exit 2; "
            + f"timeout --kill-after={TEST_TIMEOUT / 10} {int(TEST_TIMEOUT)} "
            + shlex.join(TEST_CMD)
            + '; r=$?; if [ "$r" -eq 0 ]; then exit 0; elif [ "$r" -eq 124 ]; then exit 124; else exit 1; fi'
        )
        cmd = [
            # fmt: off
            bwrap,
            "--die-with-parent",  # sandbox dies if the Python process does
            "--unshare-pid",  # automatically reap children
            "--new-session",  # sandboxed processes in their own session
            "--ro-bind", "/", "/",  # entire root read-only
            "--dev", "/dev",
            "--proc", "/proc",
            "--tmpfs", "/tmp",  # writable /tmp for ccache and other tools
            "--setenv", "TMPDIR", "/tmp",
            "--setenv", "TMP", "/tmp",
            "--setenv", "TEMP", "/tmp",
            "--setenv", "XDG_RUNTIME_DIR", "/tmp",
            "--overlay-src", str(REPO_ROOT),
            "--tmp-overlay", str(REPO_ROOT),  # writable tmpfs overlay for build artifacts
            # bind mutated source file on top of original
            *(["--bind", tmp.name, str(mutation.file)] if mutation is not None else []),
            "--",
            "sh", "-c", shell_cmd,
            # fmt: on
        ]

        streams = None if inherit_streams else subprocess.DEVNULL
        try:
            r = subprocess.run(
                cmd,
                cwd=REPO_ROOT,
                stdout=streams,
                stderr=streams,
                start_new_session=True,
                timeout=5 + 2 * TEST_TIMEOUT,
            )
        except subprocess.TimeoutExpired:
            return "timeout"

        if r.returncode == 0:
            return "passed"
        if r.returncode == 2:
            return "stillborn"
        if r.returncode == 124:
            return "timeout"
        return "failed"


def evaluate_mutants(
    mutations: list[Mutation],
    sources: dict[Path, str],
    ann: Annotations,
    maybe_bwrap: str | None = None,
    nproc: int = 1,
) -> EvalResults:
    survived: list[Mutation] = []
    triaged: list[Mutation] = []
    expected_survived: list[Mutation] = []
    counts: dict[str, int] = {"passed": 0, "failed": 0, "timeout": 0, "stillborn": 0}

    if maybe_bwrap is not None:
        evaluator = lambda m: _evaluate_one_mutant_bwrap(sources, m, maybe_bwrap)
    else:
        evaluator = lambda m: _evaluate_one_mutant(sources, m)
    with ThreadPoolExecutor(max_workers=nproc) as executor:
        outcomes = executor.map(evaluator, mutations)
        for idx, (m, outcome) in enumerate(zip(mutations, outcomes), 1):
            counts[outcome] += 1
            if outcome == "passed":
                if is_expected_survivor(m, ann):
                    tag = "survived(expected)"
                    expected_survived.append(m)
                elif (m.file, m.line_no) in ann.triaged_lines:
                    tag = "triaged"
                    triaged.append(m)
                else:
                    tag = "SURVIVED"
                    survived.append(m)
            elif outcome == "stillborn":
                tag = "stillborn"
            else:
                if is_expected_survivor(m, ann):
                    tag = f"detected(UNEXPECTED, {outcome})"
                else:
                    tag = f"detected({outcome})"
            print(f"[{idx:4d}/{len(mutations)}] {tag:22s}  {m.label()}", flush=True)

    return EvalResults(
        survived=tuple(survived),
        triaged=tuple(triaged),
        expected_survived=tuple(expected_survived),
        counts=counts,
        triaged_lines=ann.triaged_lines,
    )


def _expand_files(patterns: list[str], repo_root: Path) -> list[Path]:
    result: list[Path] = []
    for pat in patterns:
        p = Path(pat)
        if p.is_absolute():
            result.append(p)
        else:
            result.extend(sorted(repo_root.glob(pat)) or [repo_root / p])
    return result


def _check_clean_tree(files: list[Path], warn_only: bool = False) -> None:
    paths = ["--"] + [str(f) for f in files]
    for cmd in (
        ["git", "diff", "--exit-code"],
        ["git", "diff", "--cached", "--exit-code"],
    ):
        r = subprocess.run(cmd + paths, cwd=REPO_ROOT, capture_output=True)
        if r.returncode != 0:
            if warn_only:
                print(
                    "WARNING: target files have uncommitted or staged changes.",
                    file=sys.stderr,
                )
                return
            sys.exit(
                "ERROR: target files have uncommitted or staged changes.\n"
                "Commit or stash your changes before running mutation testing."
            )


def _print_eval_results(results: EvalResults) -> int:
    """Print the summary report. Returns 0 or 1 as the process exit code."""
    total = sum(results.counts.values())
    stillborn = results.counts["stillborn"]
    detected = results.counts["failed"] + results.counts["timeout"]
    scoreable = total - stillborn
    pct = 100.0 * detected / scoreable if scoreable else 0.0

    print()
    print(f"Mutations : {total}  ({stillborn} stillborn, {scoreable} scoreable)")
    print(
        f"Detected  : {detected}  ({pct:.1f}% of scoreable)"
        f"  [{results.counts['failed']} failed, {results.counts['timeout']} timeout]"
    )
    print(f"Expected  : {len(results.expected_survived)}  (known equivalent/passing)")
    print(f"Triaged   : {len(results.triaged)}  (known gap, hard to test)")
    print(f"Survived  : {len(results.survived)}  (potential test gaps)")

    if results.expected_survived:
        print()
        print("─── EXPECTED SURVIVORS " + "─" * 54)
        for m in sorted(results.expected_survived):
            print(f"  {m.label()}")

    if results.triaged:
        print()
        print("─── TRIAGED MUTANTS (acknowledged gaps) " + "─" * 37)
        for m in sorted(results.triaged):
            expl = results.triaged_lines[(m.file, m.line_no)]
            print(f"  {m.label()}")
            print(f"    # {expl}")

    if results.survived:
        print()
        print("─── SURVIVED MUTANTS (potential test gaps) " + "─" * 34)
        for m in sorted(results.survived):
            print(f"  {m.label()}")

    return 1 if results.survived else 0


# Each operator entry: (regex_pattern, replacement_string, generator_name).
RE_REPLACEMENTS: list[tuple[str, str, str]] = [
    (r"<=", ">", "operator:<= -> >"),
    (r"<=", "<", "operator:<= -> <"),
    (r">=", "<", "operator:>= -> <"),
    (r">=", ">", "operator:>= -> >"),
    (r"==", "!=", "operator:== -> !="),
    (r"!=", "==", "operator:!= -> =="),
    (r"&&", "||", "operator:&& -> ||"),
    (r"[|][|]", "&&", "operator:|| -> &&"),
    (r"(?<![<>=!-])(<)(?![<=])", ">", "operator:< -> >"),  # not <<, <=, ->
    (r"(?<![-<>=])(>)(?![>=])", "<", "operator:> -> <"),  # not >>, >=, ->
]

# --arith: arithmetic, bitwise, and shift operators.  C's overloaded syntax
# produces more stillborns (pointer *, address-of &, unary -/+).
ARITH_RE_REPLACEMENTS: list[tuple[str, str, str]] = [
    # Shifts (excluded by the single-char < and > patterns above)
    (r"<<", ">>", "arith:<< -> >>"),
    (r">>", "<<", "arith:>> -> <<"),
    # Arithmetic binary
    (r"(?<![+])([+])(?![+])", "-", "arith:+ -> -"),
    (r"(?<![-])([-])(?![->])", "+", "arith:- -> +"),  # excludes --, ->
    (r"[/]", "*", "arith:/ -> *"),  # excludes // and /*
    (r"[*]", "/", "arith:* -> /"),  # hits pointer * too
    (r"[%]", "/", "arith:% -> /"),  # excludes %=
    # Bitwise (& hits address-of, producing some stillborns)
    (r"(?<!&)(&)(?![&])", "|", "arith:& -> |"),
    (r"(?<!\|)([|])(?![|])", "&", "arith:| -> &"),
    (r"\^", "&", "arith:^ -> &"),
    (r"\^", "|", "arith:^ -> |"),
    # Increment/Decrement
    (r"[-][-]", "++", "arith:-- -> ++"),
    (r"[+][+]", "--", "arith:++ -> --"),
    (r"[-][-]", " ", "arith:-- -> nil"),
    (r"[+][+]", " ", "arith:++ -> nil"),
]


def main() -> None:
    global REPO_ROOT, TEST_TIMEOUT, BUILD_CMD, TEST_CMD
    parser = argparse.ArgumentParser(
        description="Mutation testing for the ppb src/ directory.",
    )
    parser.add_argument(
        "files",
        nargs="*",
        metavar="GLOB",
        help=f"source files or glob patterns to mutate, relative to repo root"
        f" (default: {' '.join(DEFAULT_FILES)})",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="list mutations without running make",
    )
    parser.add_argument(
        "--arith",
        action="store_true",
        help="also mutate arithmetic, bitwise, and shift operators",
    )
    parser.add_argument(
        "--dump-masked-code",
        action="store_true",
        help="print the code for each FILE and exit",
    )
    parser.add_argument(
        "--delete",
        action="store_true",
        help="also try deleting each non-structural source line",
    )
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=None,
        metavar="DIR",
        help=f"repository root used as cwd for build/test commands (default: {REPO_ROOT})",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=None,
        metavar="SECS",
        help=f"per-mutant timeout in seconds (default: {TEST_TIMEOUT})",
    )
    parser.add_argument(
        "--build-cmd",
        default=None,
        metavar="CMD",
        help=f"shell command to build the project (default: {shlex.join(BUILD_CMD)})",
    )
    parser.add_argument(
        "--test-cmd",
        default=None,
        metavar="CMD",
        help=f"shell command to run the test suite (default: {shlex.join(TEST_CMD)})",
    )
    parser.add_argument(
        "--bwrap",
        nargs="?",
        const="bwrap",
        default=None,
        metavar="EXE",
        help="use bwrap to sandbox each mutant; no source files are modified (default exe: bwrap)",
    )
    parser.add_argument(
        "-j",
        nargs="?",
        type=int,
        const=None,
        default=1,
        metavar="N",
        help="parallel evaluation workers (default: 1; -j alone uses min(16, cpu_count)); requires --bwrap for N > 1",
    )
    args = parser.parse_args()

    if args.repo_root is not None:
        REPO_ROOT = args.repo_root.resolve()
    if args.timeout is not None:
        TEST_TIMEOUT = args.timeout
    if args.build_cmd is not None:
        BUILD_CMD = shlex.split(args.build_cmd)
    if args.test_cmd is not None:
        TEST_CMD = shlex.split(args.test_cmd)

    nproc = min(16, len(os.sched_getaffinity(0)) or 1) if args.j is None else args.j
    if nproc > 1 and args.bwrap is None:
        sys.exit("ERROR: -j > 1 requires --bwrap")

    files = _expand_files(args.files if args.files else DEFAULT_FILES, REPO_ROOT)
    for f in files:
        if not f.exists():
            sys.exit(f"ERROR: {f} does not exist")

    if not args.dry_run and not args.dump_masked_code:
        _check_clean_tree(files, warn_only=args.bwrap is not None)

    operators = RE_REPLACEMENTS + (ARITH_RE_REPLACEMENTS if args.arith else [])

    sources: dict[Path, str] = {f: f.read_text() for f in files}
    infos = prepare_sources(sources)
    if args.dump_masked_code:
        for info in infos:
            if len(infos) > 1:
                print(f"=== {info.path.relative_to(REPO_ROOT)} ===")
            print(info.code, end="")
        return

    mutations = generate_mutations(infos, operators, args.delete)
    ann = parse_annotations(sources)
    print(f"Found {len(mutations)} mutation(s) across {len(files)} file(s).")

    if args.dry_run:
        for m in mutations:
            known = "(expected)" if is_expected_survivor(m, ann) else ""
            print(f"  {m.label()}  {known}".rstrip())
        return

    print("\n─── SMOKE TESTING " + "─" * 59, flush=True)
    build_outcome = _run_cmd(BUILD_CMD, inherit_streams=True)
    if build_outcome != "passed":
        sys.exit(f"Smoke test: build FAILED ({build_outcome})")
    if args.bwrap is not None:
        outcome = _evaluate_one_mutant_bwrap(
            sources, None, args.bwrap, inherit_streams=True
        )
    else:
        outcome = _evaluate_one_mutant(sources, None, inherit_streams=True)
    if outcome != "passed":
        sys.exit(f"Smoke test: baseline evaluation FAILED ({outcome})")
    print("─── SMOKE TESTING OK " + "─" * 56 + "\n")

    results = evaluate_mutants(
        mutations, sources, ann, maybe_bwrap=args.bwrap, nproc=nproc
    )
    sys.exit(_print_eval_results(results))


if __name__ == "__main__":
    main()
