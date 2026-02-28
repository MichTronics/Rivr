#!/usr/bin/env bash
# ci_determinism_check.sh — replay test golden-hash gate.
#
# Usage:
#   bash tests/ci_determinism_check.sh <golden_file>
#
# Runs tests/replay_test (must already be built), hashes its stdout, and
# compares against the first non-comment line in <golden_file>.
#
# To update the golden snapshot after an intentional behavioural change:
#   make -C tests replay
#   cd tests && ./replay_test | sha256sum | awk '{print $1}' \
#       > golden/replay_sha256.txt
set -euo pipefail

GOLDEN="${1:-tests/golden/replay_sha256.txt}"

if [[ ! -f "$GOLDEN" ]]; then
    echo "ERROR: golden file not found: $GOLDEN" >&2
    exit 1
fi

TESTS_DIR="$(cd "$(dirname "$0")" && pwd)"

if [[ ! -x "$TESTS_DIR/replay_test" ]]; then
    echo "ERROR: replay_test binary not found or not executable: $TESTS_DIR/replay_test" >&2
    exit 1
fi

# Read expected hash (first non-comment, non-blank line)
expected=""
while IFS= read -r line; do
    [[ "$line" =~ ^# ]] && continue
    [[ -z "$line" ]]    && continue
    expected="$line"
    break
done < "$GOLDEN"

if [[ -z "$expected" ]]; then
    echo "ERROR: no hash found in $GOLDEN" >&2
    exit 1
fi

# Run replay_test from its directory so relative trace paths work
actual=$(cd "$TESTS_DIR" && ./replay_test 2>&1 | sha256sum | awk '{print $1}')

echo "expected: $expected"
echo "actual:   $actual"

if [[ "$actual" == "$expected" ]]; then
    echo "OK   replay determinism check PASS"
else
    echo "FAIL replay hash mismatch — behaviour changed or non-determinism detected"
    echo "     Update golden: cd tests && ./replay_test | sha256sum | awk '{print \$1}' > golden/replay_sha256.txt"
    exit 1
fi
