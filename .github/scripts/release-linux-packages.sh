#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${script_dir}/github-progress.sh"

vlink_progress_init "Release Linux packages" \
  "Resolve package dependencies" \
  "Build deb package" \
  "Build rpm package"

vlink_progress_start "Resolve package dependencies"
if source /etc/os-release; then
  case "${VERSION_ID}" in
    20.04) export DEB_DEPENDS="libssl1.1, libsqlite3-0, libzstd1" ;;
    *) export DEB_DEPENDS="libssl3, libsqlite3-0, libzstd1" ;;
  esac
  vlink_progress_complete "Resolve package dependencies"
else
  status=$?
  vlink_progress_fail "Resolve package dependencies"
  exit "${status}"
fi

vlink_progress_run "Build deb package" bash packup/build-deb.sh .
vlink_progress_run "Build rpm package" bash packup/build-rpm.sh .
