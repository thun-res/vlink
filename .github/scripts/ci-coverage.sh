#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE:-$0}")" && pwd)"
. "$script_dir/github-progress.sh"

vlink_progress_init "CI coverage" \
    "Configure CMake" \
    "Build" \
    "Run tests" \
    "Generate coverage"

vlink_progress_start "Configure CMake"
if cmake -S . -B build-coverage ${VLINK_CI_CMAKE_ARGS}; then
    vlink_progress_complete "Configure CMake"
else
    status=$?
    vlink_progress_fail "Configure CMake"
    exit "$status"
fi

vlink_progress_run "Build" cmake --build build-coverage --parallel

export BUILD_DIR=build-coverage
vlink_progress_run "Run tests" bash "$script_dir/run-posix-ci-tests.sh"

vlink_progress_start "Generate coverage"
if cmake --build build-coverage --target coverage > build-coverage/coverage-summary.txt 2>&1; then
    :
else
    status=$?
    cat build-coverage/coverage-summary.txt
    vlink_progress_fail "Generate coverage"
    exit "$status"
fi
if [ -f build-coverage/coverage/index.html ] &&
    cp build-coverage/coverage-summary.txt build-coverage/coverage/coverage-summary.txt &&
    tail -80 build-coverage/coverage-summary.txt; then
    vlink_progress_complete "Generate coverage"
else
    status=$?
    vlink_progress_fail "Generate coverage"
    exit "$status"
fi
