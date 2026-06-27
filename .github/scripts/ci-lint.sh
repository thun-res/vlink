#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${script_dir}/github-progress.sh"

vlink_progress_init "CI lint gates" \
  "Validate workflows" \
  "Check formatting" \
  "Run cpplint"

vlink_progress_run "Validate workflows" actionlint .github/workflows/*.yml
vlink_progress_run "Check formatting" bash tools/format.sh "${GITHUB_WORKSPACE}"
vlink_progress_run "Run cpplint" bash tools/check.sh "${GITHUB_WORKSPACE}"
