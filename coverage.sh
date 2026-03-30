#!/bin/sh
set -eu

COVDIR=build/coverage

rm -rf "$COVDIR"
mkdir -p "$COVDIR"

# Build picoscope with coverage instrumentation (bypass ccache).
# Compile objects into COVDIR so .gcno/.gcda land together.
gcc -std=c2x -O0 -Iinclude/ -Wall -Wextra -Wpedantic -ggdb \
    --coverage \
    -c -o "$COVDIR/ppb.o" src/ppb.c
gcc -std=c2x -O0 -Iinclude/ -Wall -Wextra -Wpedantic -ggdb \
    --coverage \
    -c -o "$COVDIR/picoscope.o" examples/picoscope.c
gcc --coverage -o "$COVDIR/picoscope" "$COVDIR/picoscope.o" "$COVDIR/ppb.o"

gcc -std=c2x -O0 -Iinclude/ -I. -Wall -Wextra -Wpedantic -ggdb \
    --coverage \
    -c -o "$COVDIR/test_ppb.o" tests/test_ppb.c
gcc --coverage -o "$COVDIR/test_ppb" "$COVDIR/test_ppb.o" "$COVDIR/ppb.o"

# Run the test suites.
"$COVDIR/test_ppb"
sh test_picoscope.sh "$COVDIR/picoscope"

# Generate HTML report.
gcovr --root . --object-directory "$COVDIR" \
    --html-details "$COVDIR/index.html"

printf "\nCoverage report: %s/index.html\n" "$COVDIR"
