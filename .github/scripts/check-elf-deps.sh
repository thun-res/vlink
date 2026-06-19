#!/usr/bin/env bash
set -euo pipefail

status=0
checked=0
output="${TMPDIR:-/tmp}/vlink-ldd.out"

for path in "$@"; do
  checked=$((checked + 1))
  if ! ldd -r "${path}" >"${output}" 2>&1; then
    echo "::error::ELF dependency check failed: ${path}"
    cat "${output}"
    status=1
    continue
  fi
  if grep -Eq "not found|undefined symbol:" "${output}"; then
    echo "::error::ELF dependency check failed: ${path}"
    grep -E "not found|undefined symbol:" "${output}"
    status=1
  fi
done

if [ "${status}" = "0" ]; then
  echo "ELF dependency check passed for ${checked} file(s)."
fi

exit "${status}"
