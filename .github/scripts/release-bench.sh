#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${script_dir}/github-progress.sh"

vlink_progress_init "Release benchmark report" \
  "Prepare benchmark output" \
  "Run quick benchmark" \
  "Verify benchmark reports" \
  "Write badge endpoint"

bench_dir="build-doc/output/doc/bench"
bench_bin="build-doc/output/bin/vlink-bench"
badge_file="${bench_dir}/badge.json"

write_bench_badge() {
  local message="$1"
  local color="$2"
  mkdir -p "${bench_dir}"
  printf '{"schemaVersion":1,"label":"benchmark","message":"%s","color":"%s"}\n' \
    "${message}" "${color}" > "${badge_file}"
}

vlink_progress_start "Prepare benchmark output"
if test -x "${bench_bin}" &&
  mkdir -p "${bench_dir}"; then
  vlink_progress_complete "Prepare benchmark output"
else
  status=$?
  vlink_progress_fail "Prepare benchmark output"
  exit "${status}"
fi

status=0
vlink_progress_start "Run quick benchmark"
"${bench_bin}" run \
  --preset quick \
  --report html,json \
  --silent \
  --no-pager \
  --output "${bench_dir}/index" || status=$?

if [ "${status}" -ne 0 ] && [ "${status}" -ne 2 ]; then
  write_bench_badge "failing" "red"
  vlink_progress_fail "Run quick benchmark"
  exit "${status}"
fi
vlink_progress_complete "Run quick benchmark"

vlink_progress_start "Verify benchmark reports"
if test -f "${bench_dir}/index.html" &&
  test -f "${bench_dir}/index.json"; then
  vlink_progress_complete "Verify benchmark reports"
else
  status=$?
  write_bench_badge "failing" "red"
  vlink_progress_fail "Verify benchmark reports"
  exit "${status}"
fi

vlink_progress_start "Write badge endpoint"
if write_bench_badge "passing" "brightgreen"; then
  vlink_progress_complete "Write badge endpoint"
else
  status=$?
  vlink_progress_fail "Write badge endpoint"
  exit "${status}"
fi
