#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${script_dir}/github-progress.sh"

vlink_progress_init "CI macOS build/test" \
  "Install Python packages" \
  "Resolve build inputs" \
  "Configure CMake" \
  "Build" \
  "Run tests"

vlink_progress_start "Install Python packages"
if python -m pip install -q pip==26.1.2 &&
  python -m pip install -q nanobind==2.13.0; then
  vlink_progress_complete "Install Python packages"
else
  status=$?
  vlink_progress_fail "Install Python packages"
  exit "${status}"
fi

vlink_progress_start "Resolve build inputs"
if python_executable="$(python -c 'import sys; print(sys.executable)')" &&
  openssl_root="$(brew --prefix openssl@3)"; then
  vlink_progress_complete "Resolve build inputs"
else
  status=$?
  vlink_progress_fail "Resolve build inputs"
  exit "${status}"
fi

export CCACHE_DIR="${CCACHE_DIR:-${HOME}/.ccache}"
export CCACHE_COMPRESS="${CCACHE_COMPRESS:-true}"
export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-2G}"

vlink_progress_start "Configure CMake"
if cmake -S . -B build-test ${VLINK_CI_CMAKE_ARGS} ${VLINK_CI_EXTRA_CMAKE_ARGS:-} \
  -DPython_EXECUTABLE="${python_executable}" \
  -DOPENSSL_ROOT_DIR="${openssl_root}"; then
  vlink_progress_complete "Configure CMake"
else
  status=$?
  vlink_progress_fail "Configure CMake"
  exit "${status}"
fi

vlink_progress_run "Build" cmake --build build-test --parallel

export BUILD_DIR=build-test
vlink_progress_run "Run tests" bash "${script_dir}/run-posix-ci-tests.sh"
