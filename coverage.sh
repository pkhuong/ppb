#!/bin/sh
set -eu

COVDIR=build/coverage
EXTRA_FLAGS="${EXTRA_FLAGS:-} --coverage -DNDEBUG"

rm -rf "$COVDIR"
mkdir -p "$COVDIR/report"

# Build picoscope with coverage instrumentation (bypass ccache).
# Compile objects into COVDIR so .gcno/.gcda land together.
gcc -std=c2x -O0 -Iinclude/ -Wall -Wextra -Wpedantic -ggdb $EXTRA_FLAGS \
    -c -o "$COVDIR/ppb.o" src/ppb.c
gcc -std=c2x -O0 -Iinclude/ -Wall -Wextra -Wpedantic -ggdb $EXTRA_FLAGS \
    -c -o "$COVDIR/picoscope.o" examples/picoscope.c
gcc --coverage -o "$COVDIR/picoscope" "$COVDIR/picoscope.o" "$COVDIR/ppb.o"

gcc -std=c2x -O0 -Iinclude/ -I. -Wall -Wextra -Wpedantic -ggdb $EXTRA_FLAGS \
    -c -o "$COVDIR/test_ppb.o" tests/test_ppb.c
gcc --coverage -o "$COVDIR/test_ppb" "$COVDIR/test_ppb.o" "$COVDIR/ppb.o"

# Build and link the C++ test harness against the same instrumented ppb.o
# so .hpp headers get .gcno/.gcda alongside src/ppb.c.
g++ -std=c++20 -O0 -Iinclude/ -I. -Wall -Wextra -Wpedantic -ggdb $EXTRA_FLAGS \
    -c -o "$COVDIR/test_ppb_cpp.o" tests/test_ppb_cpp.cc
g++ --coverage -o "$COVDIR/test_ppb_cpp" "$COVDIR/test_ppb_cpp.o" "$COVDIR/ppb.o"

# Run the test suites.
"$COVDIR/test_ppb"
"$COVDIR/test_ppb_cpp"
sh test_picoscope.sh "$COVDIR/picoscope"

# Generate HTML report.
gcovr --root . --object-directory "$COVDIR" \
    --html-details "$COVDIR/report/index.html"

printf "\nCoverage report: %s/report/index.html\n" "$COVDIR"
