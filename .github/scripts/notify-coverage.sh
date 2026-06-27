#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${script_dir}/github-progress.sh"

vlink_progress_init "Notify coverage result" \
  "Compose email body" \
  "Send email"

vlink_progress_start "Compose email body"
if body="$(mktemp)" &&
  {
  echo "CI Coverage ${JOB_STATUS:-unknown}"
  echo
  echo "Run: ${GITHUB_SERVER_URL:-}/${GITHUB_REPOSITORY:-}/actions/runs/${GITHUB_RUN_ID:-}"
  echo "Commit: ${GITHUB_SHA:-}"
  echo
  if [ -s build-coverage/coverage-summary.txt ]; then
    cat build-coverage/coverage-summary.txt
  else
    echo "Coverage summary is unavailable."
  fi
} > "${body}"; then
  trap 'rm -f "${body}"' EXIT
  vlink_progress_complete "Compose email body"
else
  status=$?
  vlink_progress_fail "Compose email body"
  exit "${status}"
fi

vlink_progress_run "Send email" python3 .github/scripts/send-email-notification.py \
  --subject "[vlink] Coverage ${JOB_STATUS:-unknown}: ${GITHUB_REF_NAME:-}" \
  --body-file "${body}"
