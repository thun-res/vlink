#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE:-$0}")" && pwd)"
. "$script_dir/github-progress.sh"

vlink_progress_init "CI clang-tidy" \
    "Configure CMake" \
    "Build tidy target"

vlink_progress_start "Configure CMake"
if cmake -S . -B build-tidy ${VLINK_CI_CMAKE_ARGS} \
    -DCMAKE_CXX_CLANG_TIDY=clang-tidy; then
    vlink_progress_complete "Configure CMake"
else
    status=$?
    vlink_progress_fail "Configure CMake"
    exit "$status"
fi

vlink_progress_run "Build tidy target" cmake --build build-tidy --parallel
