#!/usr/bin/env bash

set -euo pipefail

REPO_DIR="${VLINK_REPO_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
BRANCH="${VLINK_REPO_BRANCH:-master}"
GIT_BIN="${GIT_BIN:-$(command -v git || true)}"

if [[ -z "${GIT_BIN}" ]]; then
  echo "[vlink-repo-update] ERROR: git executable not found" >&2
  exit 1
fi

echo "[vlink-repo-update] start: repo=${REPO_DIR} branch=${BRANCH}"

cd "${REPO_DIR}"

if ! "${GIT_BIN}" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "[vlink-repo-update] ERROR: ${REPO_DIR} is not a git repository" >&2
  exit 1
fi

echo "[vlink-repo-update] fetching all remotes ..."
"${GIT_BIN}" fetch --all --prune

echo "[vlink-repo-update] checking out ${BRANCH} ..."
"${GIT_BIN}" checkout -f "${BRANCH}"

echo "[vlink-repo-update] hard-resetting ${BRANCH} to origin/${BRANCH} (local changes discarded) ..."
"${GIT_BIN}" reset --hard "origin/${BRANCH}"

echo "[vlink-repo-update] done: now at $("${GIT_BIN}" rev-parse --short HEAD)"
