#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE:-$0}")" && pwd)"
. "$script_dir/github-progress.sh"

use_tidy_ccache="${VLINK_CI_TIDY_CCACHE:-0}"
tidy_cmake_args=(-DCMAKE_CXX_CLANG_TIDY=clang-tidy)
if [ "$use_tidy_ccache" = "1" ]; then
    mkdir -p build-tidy
    tidy_cache_version_file="$PWD/build-tidy/clang-tidy-cache-version.txt"
    {
        clang-tidy --version
        cmake --version
    } > "$tidy_cache_version_file"

    export CCACHE_PREFIX="bash $script_dir/ccache-clang-tidy.sh"
    export CCACHE_EXTRAFILES="$PWD/.clang-tidy:$script_dir/ccache-clang-tidy.sh:$tidy_cache_version_file"
    tidy_cmake_args=()
    ccache --zero-stats
fi

vlink_progress_init "CI clang-tidy" \
    "Configure CMake" \
    "Build tidy target"

vlink_progress_start "Configure CMake"
if cmake -S . -B build-tidy ${VLINK_CI_CMAKE_ARGS} \
    "${tidy_cmake_args[@]}"; then
    vlink_progress_complete "Configure CMake"
else
    status=$?
    vlink_progress_fail "Configure CMake"
    exit "$status"
fi

vlink_progress_run "Build tidy target" cmake --build build-tidy --parallel

if [ "$use_tidy_ccache" = "1" ]; then
    ccache --show-stats
fi
