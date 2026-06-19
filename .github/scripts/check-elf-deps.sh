#!/usr/bin/env bash
set -euo pipefail

status=0
output="${TMPDIR:-/tmp}/vlink-ldd.out"

for path in "$@"; do
  if ! ldd -r "${path}" >"${output}" 2>&1; then
    echo "::error::ELF dependency check failed: ${path}"
    grep -E "not found|undefined symbol:" "${output}" || head -n 40 "${output}"
    status=1
    continue
  fi
  if grep -Eq "not found|undefined symbol:" "${output}"; then
    echo "::error::ELF dependency check failed: ${path}"
    grep -E "not found|undefined symbol:" "${output}"
    status=1
  fi
done

exit "${status}"
