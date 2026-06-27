#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${script_dir}/github-progress.sh"

vlink_progress_init "Compress macOS app bundles" \
  "Locate app bundle" \
  "Resolve archive name" \
  "Create app zip"

vlink_progress_start "Locate app bundle"
darwin_dir="build-conan/packup/darwin"
app="${darwin_dir}/VLink Player.app"
if test -d "${app}"; then
  vlink_progress_complete "Locate app bundle"
else
  status=$?
  vlink_progress_fail "Locate app bundle"
  exit "${status}"
fi

vlink_progress_start "Resolve archive name"
if stem="" &&
  shopt -s nullglob; then
  for tgz in "${darwin_dir}"/vlink-desktop-*-darwin-*.tgz; do
    stem="$(basename "${tgz%.tgz}")"
    break
  done
  if [ -z "${stem}" ]; then
    if version="$(tr -d '[:space:]' < version.txt)"; then
      stem="vlink-desktop-${version}-darwin-$(uname -m)"
    else
      status=$?
      vlink_progress_fail "Resolve archive name"
      exit "${status}"
    fi
  fi
  vlink_progress_complete "Resolve archive name"
else
  status=$?
  vlink_progress_fail "Resolve archive name"
  exit "${status}"
fi

vlink_progress_run "Create app zip" ditto -c -k --sequesterRsrc --keepParent "${app}" "${darwin_dir}/${stem}-app.zip"
