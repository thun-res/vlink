#!/usr/bin/env bash
set -euo pipefail

status=0
output="${TMPDIR:-/tmp}/vlink-ldd.out"

for path in "$@"; do
  echo "ldd -r ${path}"
  if ! ldd -r "${path}" >"${output}" 2>&1; then
    cat "${output}"
    status=1
    continue
  fi
  cat "${output}"
  if grep -Eq "not found|undefined symbol:" "${output}"; then
    status=1
  fi
done

exit "${status}"
