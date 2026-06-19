#!/usr/bin/env bash
# Build and run the C++ test suite for TinySoundFont.
#
# Usage:
#   ./tests/build.sh                 # builds + runs with the bundled fixture
#   ./tests/build.sh /path/to/x.sf2  # builds + runs with a custom fixture
set -euo pipefail

cd "$(dirname "$0")/.."   # repo root

CXX="${CXX:-clang++}"
CXXFLAGS="${CXXFLAGS:--std=c++17 -Wall -Wextra -O2}"

echo "Compiling tests/test_tsf.cpp ..."
"$CXX" $CXXFLAGS tests/test_tsf.cpp -o tests/test_tsf -lm

FIXTURE="${1:-examples/florestan-subset.sf2}"
echo "Running tests with fixture: $FIXTURE"
exec ./tests/test_tsf "$FIXTURE"
