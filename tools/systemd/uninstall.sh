#!/usr/bin/env bash

set -euo pipefail

UNIT_DIR="${UNIT_DIR:-/etc/systemd/system}"

SUDO=""
SYSTEMCTL="systemctl"

case "${UNIT_DIR}" in
  *systemd/user*)
    SYSTEMCTL="systemctl --user"
    ;;
  *)
    if [[ "$(id -u)" -ne 0 ]]; then
      SUDO="sudo"
      SYSTEMCTL="sudo systemctl"
      sudo -v
    fi
    ;;
esac

${SYSTEMCTL} disable --now vlink-repo-update.timer || true
${SUDO} rm -f "${UNIT_DIR}/vlink-repo-update.timer" "${UNIT_DIR}/vlink-repo-update.service"
${SYSTEMCTL} daemon-reload

echo "[uninstall] removed vlink-repo-update.timer / .service"
