#!/usr/bin/env bash

set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC_DIR="${REPO_DIR}/tools/systemd"
UNIT_DIR="${UNIT_DIR:-/etc/systemd/system}"

case "${UNIT_DIR}" in
  *systemd/user*)
    USER_MODE=1
    ;;
  *)
    USER_MODE=0
    ;;
esac

SUDO=""
SYSTEMCTL="systemctl"

if [[ "${USER_MODE}" -eq 1 ]]; then
  SYSTEMCTL="systemctl --user"
elif [[ "$(id -u)" -ne 0 ]]; then
  SUDO="sudo"
  SYSTEMCTL="sudo systemctl"
  sudo -v
fi

chmod +x "${SRC_DIR}/vlink-repo-update.sh"

repo_esc="${REPO_DIR//\\/\\\\}"
repo_esc="${repo_esc//&/\\&}"
repo_esc="${repo_esc//#/\\#}"

service_content="$(sed "s#@REPO_DIR@#${repo_esc}#g" "${SRC_DIR}/vlink-repo-update.service")"
timer_content="$(sed "s#@REPO_DIR@#${repo_esc}#g" "${SRC_DIR}/vlink-repo-update.timer")"

if [[ "${USER_MODE}" -eq 0 ]]; then
  owner="$(stat -c '%U' "${REPO_DIR}")"
  group="$(stat -c '%G' "${REPO_DIR}")"
  service_content="$(printf '%s\n' "${service_content}" | sed "/^\[Service\]\$/a User=${owner}\nGroup=${group}")"
  echo "[install] service will run as repo owner: ${owner}:${group}"
fi

echo "[install] repo dir : ${REPO_DIR}"
echo "[install] unit dir : ${UNIT_DIR}"

${SUDO} install -d "${UNIT_DIR}"

printf '%s\n' "${service_content}" | ${SUDO} tee "${UNIT_DIR}/vlink-repo-update.service" >/dev/null
printf '%s\n' "${timer_content}" | ${SUDO} tee "${UNIT_DIR}/vlink-repo-update.timer" >/dev/null

${SYSTEMCTL} daemon-reload
${SYSTEMCTL} enable --now vlink-repo-update.timer

echo "[install] done. next runs:"
${SYSTEMCTL} list-timers vlink-repo-update.timer --no-pager
