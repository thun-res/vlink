#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${script_dir}/github-progress.sh"

vlink_progress_init "AI code review request" \
  "Validate pull request number" \
  "Load pull request metadata" \
  "Post review request"

vlink_progress_start "Validate pull request number"
pr_number="${1:-}"
case "${pr_number}" in
  ''|*[!0-9]*)
    echo "::error::A positive pull request number is required."
    vlink_progress_fail "Validate pull request number"
    exit 1
    ;;
  *)
    vlink_progress_complete "Validate pull request number"
    ;;
esac

vlink_progress_start "Load pull request metadata"
if gh pr view "${pr_number}" --repo "${GITHUB_REPOSITORY}" --json number >/dev/null; then
  vlink_progress_complete "Load pull request metadata"
else
  status=$?
  vlink_progress_fail "Load pull request metadata"
  exit "${status}"
fi
vlink_progress_run "Post review request" \
  gh pr comment "${pr_number}" --repo "${GITHUB_REPOSITORY}" --body "@codex review"
