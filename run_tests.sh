#!/bin/bash
# run_tests.sh — 构建并运行所有测试
set -e

BUILD_DIR="build"
cd "$(dirname "$0")"

echo "=== Building ==="
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake .. -DCMAKE_BUILD_TYPE=Debug > /dev/null 2>&1
make -j$(nproc) 2>&1 | tail -3

echo ""
echo "=== Running Tests ==="

PASSED=0
FAILED=0
ERRORS=""

for test_bin in test_*; do
    [ -x "$test_bin" ] || continue
    echo "--- $test_bin ---"
    if ./"$test_bin"; then
        PASSED=$((PASSED + 1))
    else
        FAILED=$((FAILED + 1))
        ERRORS="$ERRORS  $test_bin\n"
    fi
    echo ""
done

echo "=== Summary ==="
echo "Passed: $PASSED"
echo "Failed: $FAILED"

if [ $FAILED -gt 0 ]; then
    echo -e "Failed tests:\n$ERRORS"
    exit 1
fi

echo "All tests PASSED!"
