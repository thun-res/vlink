#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE:-$0}")" && pwd)"
. "$script_dir/github-progress.sh"

vlink_progress_init "Generate coverage badge" \
    "Parse line coverage" \
    "Write badge endpoint"

summary_file="${COVERAGE_SUMMARY_FILE:-build-doc/output/doc/coverage/coverage-summary.txt}"
badge_file="${COVERAGE_BADGE_FILE:-build-doc/output/doc/coverage/badge.json}"

vlink_progress_start "Parse line coverage"
if [ ! -f "$summary_file" ]; then
    echo "Coverage summary not found: $summary_file" >&2
    vlink_progress_fail "Parse line coverage"
    exit 1
fi

# Extract the overall line-coverage percentage. The summary format differs
# between gcovr (`lines: 82.5% (1234 out of 1496)`) and lcov/genhtml
# (`  lines......: 82.5% (1234 of 1496 lines)`), so match the `lines` keyword
# followed by a percentage and keep only the numeric value.
line_pct="$(grep -iE "^[[:space:]]*lines[[:space:].]*:" "$summary_file" | tail -1 |
    sed -E "s/.*:[[:space:]]*([0-9.]+)%.*/\1/" | head -1)"

if [ -z "$line_pct" ]; then
    echo "Could not parse line coverage from $summary_file" >&2
    vlink_progress_fail "Parse line coverage"
    exit 1
fi
vlink_progress_complete "Parse line coverage"

vlink_progress_start "Write badge endpoint"
# Map the percentage onto the standard shields.io colour ramp. Truncate any
# fractional part so the integer comparison stays valid under `set -e`.
pct_int="${line_pct%.*}"
if [ "$pct_int" -ge 90 ]; then
    color="brightgreen"
elif [ "$pct_int" -ge 80 ]; then
    color="green"
elif [ "$pct_int" -ge 70 ]; then
    color="yellowgreen"
elif [ "$pct_int" -ge 60 ]; then
    color="yellow"
elif [ "$pct_int" -ge 50 ]; then
    color="orange"
else
    color="red"
fi

# Emit a shields.io "endpoint" schema document that README badges reference via
# https://img.shields.io/endpoint?url=<published badge.json URL>.
if mkdir -p "$(dirname "$badge_file")" &&
    printf '{"schemaVersion":1,"label":"coverage","message":"%s%%","color":"%s"}\n' \
        "$line_pct" "$color" > "$badge_file"; then
    vlink_progress_complete "Write badge endpoint"
else
    status=$?
    vlink_progress_fail "Write badge endpoint"
    exit "$status"
fi
