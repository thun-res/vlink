#!/usr/bin/env bash
set -euo pipefail

status=0
output="${TMPDIR:-/tmp}/vlink-otool.out"

for path in "$@"; do
  if ! otool -L "${path}" >"${output}" 2>&1; then
    echo "::error::Mach-O dependency check failed: ${path}"
    head -n 40 "${output}"
    status=1
    continue
  fi
  if grep -Eq "not found|cannot open" "${output}"; then
    echo "::error::Mach-O dependency check failed: ${path}"
    grep -E "not found|cannot open" "${output}"
    status=1
  fi
done

exit "${status}"
