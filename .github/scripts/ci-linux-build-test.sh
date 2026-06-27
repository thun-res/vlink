#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${script_dir}/github-progress.sh"

vlink_progress_init "CI Linux build/test" \
  "Configure CMake" \
  "Build" \
  "Run tests"

vlink_progress_start "Configure CMake"
if cmake -S . -B build-test ${VLINK_CI_CMAKE_ARGS} ${VLINK_CI_EXTRA_CMAKE_ARGS:-} \
  -DPython_EXECUTABLE="$(command -v python3)"; then
  vlink_progress_complete "Configure CMake"
else
  status=$?
  vlink_progress_fail "Configure CMake"
  exit "${status}"
fi

vlink_progress_run "Build" cmake --build build-test --parallel

export BUILD_DIR=build-test
export ASAN_OPTIONS="${ASAN_OPTIONS:+${ASAN_OPTIONS}:}detect_odr_violation=0"
vlink_progress_run "Run tests" bash "${script_dir}/run-posix-ci-tests.sh"
