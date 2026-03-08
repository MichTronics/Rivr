#!/usr/bin/env bash
# ci_size_check.sh — host binary size regression gate.
#
# Usage:
#   bash tests/ci_size_check.sh <baseline_file> <tolerance_pct>
#
# Reads <baseline_file> lines of the form:
#   <binary_name> <baseline_bytes>
# Measures the current size of each binary in tests/<binary_name>.
# Fails (exit 1) if any binary's current size exceeds baseline × (1 + tolerance%).
#
# To update the baseline after an intentional size increase:
#   make -C tests all
#   bash tests/ci_size_check.sh --update tests/baselines/host_sizes.txt
set -euo pipefail

BASELINE="${1:-tests/baselines/host_sizes.txt}"
TOLERANCE="${2:-5}"     # percent, no % sign

if [[ "${BASELINE}" == "--update" ]]; then
    # Regenerate mode: rewrite the baseline from current binaries.
    BASELINE="${2}"
    TESTS_DIR="$(dirname "$BASELINE")/../"   # tests/ relative to baselines/
    TESTS_DIR="$(cd "$(dirname "$BASELINE")/.." && pwd)"
    tmpfile="$(mktemp)"
    echo "# Auto-regenerated $(date -u +%Y-%m-%d)" > "$tmpfile"
    while IFS= read -r line; do
        [[ "$line" =~ ^# ]] && echo "$line" >> "$tmpfile" && continue
        [[ -z "$line" ]]    && echo ""       >> "$tmpfile" && continue
        name=$(echo "$line" | awk '{print $1}')
        bin="$TESTS_DIR/$name"
        if [[ -f "$bin" ]]; then
            sz=$(stat -c%s "$bin")
            echo "$name $sz" >> "$tmpfile"
        else
            echo "$name 0"   >> "$tmpfile"
        fi
    done < "$BASELINE"
    mv "$tmpfile" "$BASELINE"
    echo "Baseline updated: $BASELINE"
    exit 0
fi

if [[ ! -f "$BASELINE" ]]; then
    echo "ERROR: baseline file not found: $BASELINE" >&2
    exit 1
fi

TESTS_DIR="$(cd "$(dirname "$0")" && pwd)"   # tests/ absolute path

passed=0
failed=0

while IFS= read -r line; do
    # Skip comment / blank lines
    [[ "$line" =~ ^# ]] && continue
    [[ -z "$line" ]]    && continue

    name=$(echo "$line" | awk '{print $1}')
    base=$(echo "$line" | awk '{print $2}')

    # Skip placeholder entries (size == 0)
    [[ "$base" -eq 0 ]] && continue

    bin="$TESTS_DIR/$name"
    if [[ ! -f "$bin" ]]; then
        echo "WARN: binary not found, skipping: $bin"
        continue
    fi

    cur=$(stat -c%s "$bin")
    max=$(( base + base * TOLERANCE / 100 ))

    if (( cur > max )); then
        echo "FAIL: $name  cur=${cur}B  baseline=${base}B  limit=${max}B (+${TOLERANCE}%)"
        (( failed++ )) || true
    else
        pct=$(( (cur - base) * 100 / base ))
        echo "OK   $name  cur=${cur}B  baseline=${base}B  delta=${pct}%"
        (( passed++ )) || true
    fi
done < "$BASELINE"

echo ""
echo "size-check: ${passed} OK  ${failed} FAIL"
[[ "$failed" -eq 0 ]]
