#!/bin/bash
# Run all unit tests

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BIN_DIR="$PROJECT_ROOT/test/build/bin"
BUILD_DIR="$PROJECT_ROOT/test/build"

echo "=== Running Snapix Unit Tests ==="
echo "Binary directory: $BIN_DIR"
echo ""

if [ ! -d "$BIN_DIR" ] || [ ! -f "$BUILD_DIR/CTestTestfile.cmake" ]; then
    echo "ERROR: Build directory not found. Run build_tests.sh first."
    exit 1
fi

# CTest runs only targets configured by the current source tree.  Iterating the
# output directory also executed stale binaries left behind by renamed/removed
# tests and could make CI results misleading.
ctest --test-dir "$BUILD_DIR" --output-on-failure
