#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${script_dir}/github-progress.sh"

vlink_progress_init "Notify release result" \
  "Resolve release status" \
  "Compose email body" \
  "Send email"

vlink_progress_start "Resolve release status"
status=success
event="${GITHUB_EVENT_NAME:-}"
for result in \
  "${DOCKER_IMAGES_RESULT:-}" \
  "${PORTABLE_LINUX_RESULT:-}" \
  "${PORTABLE_MACOS_RESULT:-}" \
  "${PORTABLE_WINDOWS_RESULT:-}" \
  "${LINUX_PACKAGES_RESULT:-}" \
  "${PUBLISH_GITHUB_RELEASE_RESULT:-}" \
  "${COVERAGE_RESULT:-}" \
  "${PUBLISH_DOCS_RESULT:-}"; do
  case "${result}" in
    failure|cancelled)
      status=failure
      ;;
    skipped)
      if [ "${event}" = "release" ]; then
        status=failure
      fi
      ;;
  esac
done
vlink_progress_complete "Resolve release status"

vlink_progress_start "Compose email body"
if body="$(mktemp)" &&
  {
  echo "Release ${status}"
  echo
  echo "Run: ${GITHUB_SERVER_URL:-}/${GITHUB_REPOSITORY:-}/actions/runs/${GITHUB_RUN_ID:-}"
  echo "Ref: ${GITHUB_REF_NAME:-}"
  echo "Commit: ${GITHUB_SHA:-}"
  echo
  echo "docker-images: ${DOCKER_IMAGES_RESULT:-}"
  echo "portable-linux: ${PORTABLE_LINUX_RESULT:-}"
  echo "portable-macos: ${PORTABLE_MACOS_RESULT:-}"
  echo "portable-windows: ${PORTABLE_WINDOWS_RESULT:-}"
  echo "linux-packages: ${LINUX_PACKAGES_RESULT:-}"
  echo "publish-github-release: ${PUBLISH_GITHUB_RELEASE_RESULT:-}"
  echo "coverage: ${COVERAGE_RESULT:-}"
  echo "publish-docs: ${PUBLISH_DOCS_RESULT:-}"
} > "${body}"; then
  trap 'rm -f "${body}"' EXIT
  vlink_progress_complete "Compose email body"
else
  status_code=$?
  vlink_progress_fail "Compose email body"
  exit "${status_code}"
fi

vlink_progress_run "Send email" python3 .github/scripts/send-email-notification.py \
  --subject "[vlink] Release ${status}: ${GITHUB_REF_NAME:-}" \
  --body-file "${body}"
