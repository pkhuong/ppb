#!/bin/sh
set -eu

TESTDATA="$(dirname "$0")/testdata"

# Parse args: [-g PROTOSCOPE [PICOSCOPE]] | [PICOSCOPE]
REGEN=""
if [ "${1:-}" = "-g" ]; then
    shift
    REGEN="${1:?Usage: test_picoscope.sh -g PROTOSCOPE [PICOSCOPE]}"
    shift
fi

PICOSCOPE="${1:-build/picoscope}"

# Regenerate mode: overwrite all .expected and .expected-error files and exit.
if [ -n "$REGEN" ]; then
    count=0
    for hex in "$TESTDATA"/*.hex; do
        xxd -r -p "$hex" | "$REGEN" > "${hex%.hex}.expected"
        count=$((count + 1))
    done

    for hex in "$TESTDATA"/invalid/*.hex; do
        [ -f "$hex" ] || continue
        xxd -r -p "$hex" | "$PICOSCOPE" -p 2>"${hex%.hex}.expected-error" >/dev/null || true
        count=$((count + 1))
    done

    printf "Regenerated %d expected outputs\n" "$count"
    exit 0
fi

# Test mode: compare picoscope -p output against .expected files.
PASS=0
FAIL=0

for hex in "$TESTDATA"/*.hex; do
    name="${hex%.hex}"
    base="$(basename "$name")"
    expected="${name}.expected"

    if [ ! -f "$expected" ]; then
        FAIL=$((FAIL + 1))
        printf "FAIL: %s (missing %s)\n" "$base" "$expected"
        continue
    fi

    actual=$(xxd -r -p "$hex" | "$PICOSCOPE" -p)
    expected_text=$(cat "$expected")

    if [ "$actual" = "$expected_text" ]; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        printf "FAIL: %s\n" "$base"
        printf '%s\n' "$actual" | diff -u "$expected" -
    fi
done

# Invalid input tests: verify picoscope rejects with correct error message.
for hex in "$TESTDATA"/invalid/*.hex; do
    [ -f "$hex" ] || continue
    name="${hex%.hex}"
    base="$(basename "$name")"
    expected_err="${name}.expected-error"

    if [ ! -f "$expected_err" ]; then
        FAIL=$((FAIL + 1))
        printf "FAIL: invalid/%s (missing %s)\n" "$base" "$expected_err"
        continue
    fi

    if xxd -r -p "$hex" | "$PICOSCOPE" -p >/dev/null 2>/dev/null; then
        FAIL=$((FAIL + 1))
        printf "FAIL: invalid/%s (expected failure but succeeded)\n" "$base"
        continue
    fi

    actual_err=$(xxd -r -p "$hex" | "$PICOSCOPE" -p 2>&1 >/dev/null) || true
    expected_err_text=$(cat "$expected_err")

    if [ "$actual_err" = "$expected_err_text" ]; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        printf "FAIL: invalid/%s\n" "$base"
        printf "  expected: %s\n" "$expected_err_text"
        printf "  actual:   %s\n" "$actual_err"
    fi
done

printf "\n%d passed, %d failed\n" "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
