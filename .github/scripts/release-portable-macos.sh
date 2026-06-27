#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${script_dir}/github-progress.sh"

stderr_log="$(mktemp)"
trap 'rm -f "${stderr_log}"' EXIT

status=0
progress_steps=()
if [ -n "${CMAKE_GENERATOR:-}" ]; then
  progress_steps+=("Configure Conan generator")
fi
progress_steps+=(
  "Build Conan package"
  "Filter packaging diagnostics"
  "Check runtime closure"
)

vlink_progress_init "Release portable macOS" "${progress_steps[@]}"

if [ -n "${CMAKE_GENERATOR:-}" ]; then
  vlink_progress_start "Configure Conan generator"
  if conan_home="${CONAN_HOME:-${HOME}/.conan2}" &&
    mkdir -p "${conan_home}" &&
    printf '&:tools.cmake.cmaketoolchain:generator=%s\n' "${CMAKE_GENERATOR}" >> "${conan_home}/global.conf"; then
    vlink_progress_complete "Configure Conan generator"
  else
    status=$?
    vlink_progress_fail "Configure Conan generator"
    exit "${status}"
  fi
fi

vlink_progress_start "Build Conan package"
bash packup/build-conan.sh . arm64 2> "${stderr_log}" || status=$?
if [ "${status}" -eq 0 ]; then
  vlink_progress_complete "Build Conan package"
else
  vlink_progress_fail "Build Conan package"
fi

vlink_progress_start "Filter packaging diagnostics"
if awk '
  /strip: error: symbols referenced by indirect symbol table entries/ {drop=1; next}
  drop && /^__/ {next}
  {drop=0; print}
' "${stderr_log}" >&2; then
  vlink_progress_complete "Filter packaging diagnostics"
else
  filter_status=$?
  vlink_progress_fail "Filter packaging diagnostics"
  exit "${filter_status}"
fi

if [ "${status}" -ne 0 ]; then
  vlink_progress_fail "Build Conan package"
  exit "${status}"
fi

vlink_progress_run "Check runtime closure" bash "${script_dir}/check-macos-runtime-closure.sh"
