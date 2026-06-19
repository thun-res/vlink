#!/usr/bin/env bash

set -uo pipefail

usage() {
  printf 'Usage:\n  format.sh {project dir}\n'
}

if [ "$#" -eq 0 ] || [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
  usage
  exit 0
fi

project_dir="$1"
if [ ! -d "${project_dir}" ]; then
  echo "Error: project directory does not exist: ${project_dir}" >&2
  exit 1
fi

install_tool() {
  local tool="$1"
  local package="$2"

  if command -v "${tool}" >/dev/null 2>&1; then
    return 0
  fi

  echo "Missing ${tool}; installing ${package} with pip." >&2
  if ! python3 -m pip install "${package}" --user; then
    echo "Error: failed to install ${package}; ${tool} is unavailable." >&2
    exit 1
  fi
}

print_tool_version() {
  local tool="$1"

  echo "${tool}: $("${tool}" --version 2>&1 | head -n 1)"
}

run_formatter() {
  local label="$1"
  local tool="$2"
  shift 2
  local files=("$@")
  local status

  printf '\n=== %s ===\n\n' "${label}"
  print_tool_version "${tool}"
  echo "Project: ${project_dir}"
  echo "Files: ${#files[@]}"

  if [ "${#files[@]}" -eq 0 ]; then
    echo "No files matched ${label}; skipping."
    return 0
  fi

  "${tool}" -i "${files[@]}"
  status=$?
  if [ "${status}" -ne 0 ]; then
    echo "Error: ${tool} failed with exit code ${status}." >&2
    echo "Project: ${project_dir}" >&2
    echo "Files attempted: ${#files[@]}" >&2
    echo "${tool} prints parse errors above when a specific file is invalid." >&2
    return "${status}"
  fi
}

install_tool clang-format clang-format
install_tool cmake-format cmake-format

srcs=()
while IFS= read -r -d '' file; do
  srcs+=("${file}")
done < <(
  find "${project_dir}" -type f \
    -not -path "*/thirdparty/*" \
    -not -path "*/build/*" \
    -not -path "*/build-*" \
    -not -path "*/builtin/*" \
    -not -path "*/prebuilt/*" \
    -not -path "*/android-bp/*" \
    \( -name "*.h" -o -name "*.hpp" -o -name "*.cc" -o -name "*.cpp" \) \
    -print0
)

confs=()
while IFS= read -r -d '' file; do
  confs+=("${file}")
done < <(
  find "${project_dir}" -type f \
    -not -path "*/thirdparty/*" \
    -not -path "*/build/*" \
    -not -path "*/build-*" \
    -not -path "*/builtin/*" \
    -not -path "*/prebuilt/*" \
    -not -path "*/android-bp/*" \
    \( -name "CMakeLists.txt" -o -name "*.cmake" -o -name "*.cmake.in" \) \
    -print0
)

if ! run_formatter CLANG-FORMAT clang-format "${srcs[@]}"; then
  exit 1
fi

if ! run_formatter CMAKE-FORMAT cmake-format "${confs[@]}"; then
  exit 1
fi

if ! git -C "${project_dir}" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "Error: ${project_dir} is not a git worktree; cannot check formatting diff." >&2
  exit 1
fi

if git -C "${project_dir}" diff --quiet; then
  exit 0
fi

printf '\nFiles changed after formatting:\n\n' >&2
git -C "${project_dir}" diff --name-status -- . ':!CMakeUserPresets.json' >&2 || \
  git -C "${project_dir}" diff --name-status >&2

echo >&2
echo "Error: formatting is not clean; run tools/format.sh and commit the resulting changes." >&2
exit 1
