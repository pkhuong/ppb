# PPB Security

PPB is an allocation-free C11 lexer for protobuf binary encoding (v2/v3,
no groups), intended for use on untrusted inputs. This document
describes what that means concretely: the threat model, what is
verified, what readers can re-verify themselves, what callers are
responsible for, and what is out of scope.

## Threat model

PPB distinguishes between **untrusted** input data and **trusted**
caller-supplied data structures.  The preconditions on trusted data
structures are split into two tiers: a short list required for
UB-freedom, and a longer list required for correct output.

### Untrusted (arbitrary bytes are safe)

- The wire bytes themselves: `ppb_buf.buf[0 .. size)` may be arbitrary
  attacker-controlled input, for both the `ppb_prescan` and `ppb_lexn`
  families of functions.
- The helpers `ppb_decode_varint` and `ppb_zag` may be called on
  attacker-controlled input directly, outside any prescan / lexn loop.

### Trusted (required for UB-freedom)

PPB is expected to terminate with the documented time complexity
(linear in the number of message fields times logarithmic in the
number of tags) and avoid undefined behavior and memory safety issues
as long as:

- `tags[]`, `fields[]`, and the input buffer point to valid
  non-aliasing memory of the size implied by `num_fields` and
  `ppb_buf.size` respectively. The `restrict` qualifiers document
  the non-aliasing requirement.
- `fields[]` and the `struct ppb_buf` referred to by the `buf`
  argument to `ppb_lexn*` are writable; `tags[]` and the bytes backing
  `ppb_buf.buf` may be read-only.
- `ppb_buf.size ≤ PTRDIFF_MAX`. Objects larger than `PTRDIFF_MAX`
  introduce well known UB; see [Pascal Cuoq's writeup](https://www.trust-in-soft.com/resources/blogs/2016-05-20-objects-larger-than-ptrdiff_max-bytes).

N.B., trusted input that satisfies this subsection but not the next
*may* trigger assertion failures when PPB is built with assertions.

### Trusted (required for correct results, but not for UB-freedom)

- `fields[]` is zero-initialized before the first call to `ppb_prescan`
  or `ppb_lexn` (no need to reinitialize between the two) for a given
  message.
- `tags[]` is sorted ascending by `.bits`, with no entry having
  `.bits < 8` (i.e., no field number 0). PPB exposes
  `ppb_validate_tags` for this.

A `tags[]` array that violates the ordering or sentinel rule produces
wrong output, **not UB**.  It may result in an assertion failure,
when PPB is built with assertions enabled.

The libFuzzer harness `fuzz_invalid_tags` in `fuzz/fuzz_ppb.c`
deliberately skips `ppb_validate_tags` and runs the public API on
adversarial tag arrays under ASan + UBSan.

The two-tier split between UB-freedom preconditions and correctness
preconditions is also checked statically by
[Frama-C's WP](https://www.frama-c.com/fc-plugins/wp.html), via
behaviors in the ACSL contracts.

### Progress guarantees even when trust is broken

`ppb_lexn` and its variants always make progress (always consume at
least one byte or return an error, unless the input is empty) even
when passed an invalid `tags[]` array.  This is asserted as the
`progress` ACSL `behavior` clause on each `ppb_lexn*` and dynamically
checked in `fuzz_invalid_tags`.

## Memory-safety guarantees

PPB carries pervasive ACSL annotations. `make wp` discharges every
memory-safety, termination, and behavioral goal except:

- The `admit`ted properties listed below. Each is manually verified as
  sound, and the spirit of every claim is dynamically asserted in the
  libFuzzer harness.
- Unknown goal status for `__builtin_usubl_overflow` and
  `__builtin_ctzll` (see `wp.csv`). These are builtin postconditions
  inherited from Frama-C that can't be discharged because the stubs
  lack definitions.  We just have to trust that their contracts
  correctly model gcc's interpretation.

All unit, golden, and fuzz tests additionally run under ASan + UBSan
in CI on gcc and clang, on x86-64 (with BMI2 fast path), aarch64, i686
(`-m32`), and s390x (for big-endian coverage via QEMU, UBSan-only).

## Caller responsibilities

When parsing untrusted input, callers also have to wield PPB safely.

1. **Cap nesting depth across submessages.** PPB is iterative within a
   single message level; submessages require recursive descent in
   caller code. Unbounded recursion is a constant source of CVEs
   for protobuf libraries (see CVE-2024-7254, CVE-2026-0994).
2. **Choose budgets explicitly.** `max_lexed_fields` and the
   `_with_hard_limit` / `_with_soft_limit` byte caps are correctness /
   policy knobs. They are *not* memory-safety constraints: the input
   buffer may be read up to the `ppb_buf`'s size, regardless of the
   soft/hard limits.
3. **Validate wire-controlled sizes before allocating.** PPB allocates
   no memory itself, but callers might. When a `PPB_WIRE_LEN` field is
   decoded, `field.v.payload.size` reflects an attacker-controlled
   length prefix. Validate it before using it as an allocation size.
4. **Call `ppb_validate_tags`** once at startup on every static
   `tags[]` array.
5. **Do not alias** `tags[]`, `fields[]`, or the input buffer with one
   another (in theory, it's safe to alias read-only buffers, but
   that would be weird).
6. **Zero-initialize `fields[]`** before the first call on a given
   message.

## Out of scope

PPB does **not** defend against the following:

- **Resource exhaustion via caller-chosen budgets.** `num_fields`,
  `max_lexed_fields`, and any limit value passed in are caller policy.
- **Schema-level semantic validity.** PPB enforces correct wire format
  only.  Type confusion across fields, oneof violations, missing
  required fields, and similar concerns are caller responsibilities.
- **Side-channel resistance.** PPB is not constant-time; varint length
  and field-dispatch paths are data-dependent.
- **Mixed-endian / VAX-endian hosts.** Unsupported. Big-endian is
  best-effort: `ppb_field_value` has BE-aware overlays and CI runs
  s390x via QEMU, but there is no native (non-emulated) BE host
  coverage and Frama-C still proves under `gcc_x86_64`.
- **Compilers other than gcc / clang.** May or may not work; MSVC is
  currently unsupported.
- **Concurrent mutation of the input buffer** while parsing. The
  buffer must be read-only for the duration of the call.

## Reporting a vulnerability

PPB is a low-stakes project. If you're unsure whether something
warrants a security report, opening a regular GitHub issue is fine.

Otherwise, use either:

- GitHub's private vulnerability reporting on this repository
  (Security tab -> "Privately report a vulnerability"), or
- email <pvk@pvk.ca>.

Response will be best-effort with no SLA.
