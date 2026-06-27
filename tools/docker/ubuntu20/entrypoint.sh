#!/usr/bin/env bash
set -euo pipefail

CONTAINER_USER_NAME="${CONTAINER_USER_NAME:-ubuntu}"
CONTAINER_GROUP_NAME="${CONTAINER_GROUP_NAME:-$CONTAINER_USER_NAME}"
HOST_USER_UID="${HOST_USER_UID:-1000}"
HOST_USER_GID="${HOST_USER_GID:-$HOST_USER_UID}"
USER_PASSWORD_VALUE="${CONTAINER_USER_PASSWORD:-${USER_PASSWORD:-123}}"
ROOT_PASSWORD_VALUE="${ROOT_PASSWORD:-$USER_PASSWORD_VALUE}"
HOME_DIR="/home/$CONTAINER_USER_NAME"
WORK_DIR="${WORK_DIR:-/work}"
USER_SHELL="${CONTAINER_USER_SHELL:-/usr/bin/zsh}"

die() {
    echo "entrypoint: $*" >&2
    exit 1
}

validate_name() {
    [[ "$2" =~ ^[a-z_][a-z0-9_-]*[$]?$ ]] || die "Invalid $1: $2"
}

validate_uint() {
    case "$2" in
        ''|*[!0-9]*) die "Invalid $1: $2" ;;
    esac
}

run_as_user() {
    export HOME="$HOME_DIR" USER="$CONTAINER_USER_NAME" LOGNAME="$CONTAINER_USER_NAME" SHELL="$USER_SHELL"
    if command -v gosu >/dev/null 2>&1; then
        exec gosu "$CONTAINER_USER_NAME" "$@"
    fi
    exec runuser -u "$CONTAINER_USER_NAME" -- "$@"
}

if [ "$(id -u)" != "0" ]; then
    exec "$@"
fi

validate_name CONTAINER_USER_NAME "$CONTAINER_USER_NAME"
validate_uint HOST_USER_UID "$HOST_USER_UID"
validate_uint HOST_USER_GID "$HOST_USER_GID"

if [ "$HOST_USER_UID" = "0" ]; then
    export HOME=/root USER=root LOGNAME=root
    exec "$@"
fi

if id "$CONTAINER_USER_NAME" >/dev/null 2>&1 \
    && [ "$(id -u "$CONTAINER_USER_NAME")" = "$HOST_USER_UID" ] \
    && [ "$(id -g "$CONTAINER_USER_NAME")" = "$HOST_USER_GID" ]; then
    run_as_user "$@"
fi

validate_name CONTAINER_GROUP_NAME "$CONTAINER_GROUP_NAME"

group_for_gid="$(getent group "$HOST_USER_GID" | cut -d: -f1)" || group_for_gid=""
if [ -n "$group_for_gid" ]; then
    CONTAINER_GROUP_NAME="$group_for_gid"
elif getent group "$CONTAINER_GROUP_NAME" >/dev/null; then
    groupmod -g "$HOST_USER_GID" "$CONTAINER_GROUP_NAME"
else
    groupadd -g "$HOST_USER_GID" "$CONTAINER_GROUP_NAME"
fi

user_for_uid="$(getent passwd "$HOST_USER_UID" | cut -d: -f1)" || user_for_uid=""
if id "$CONTAINER_USER_NAME" >/dev/null 2>&1; then
    if [ -n "$user_for_uid" ] && [ "$user_for_uid" != "$CONTAINER_USER_NAME" ]; then
        die "UID $HOST_USER_UID is already used by user $user_for_uid"
    fi
    usermod -u "$HOST_USER_UID" -g "$HOST_USER_GID" -d "$HOME_DIR" -s "$USER_SHELL" "$CONTAINER_USER_NAME"
else
    if [ -n "$user_for_uid" ]; then
        die "UID $HOST_USER_UID is already used by user $user_for_uid"
    fi
    useradd -M -u "$HOST_USER_UID" -g "$HOST_USER_GID" -d "$HOME_DIR" -s "$USER_SHELL" "$CONTAINER_USER_NAME"
fi

printf '%s:%s\n' root "$ROOT_PASSWORD_VALUE" | chpasswd
printf '%s:%s\n' "$CONTAINER_USER_NAME" "$USER_PASSWORD_VALUE" | chpasswd

if [ ! -f "/etc/sudoers.d/$CONTAINER_USER_NAME" ]; then
    mkdir -p /etc/sudoers.d
    printf '%s ALL=(ALL) ALL\n' "$CONTAINER_USER_NAME" > "/etc/sudoers.d/$CONTAINER_USER_NAME"
    chmod 0440 "/etc/sudoers.d/$CONTAINER_USER_NAME"
fi

mkdir -p "$HOME_DIR" "$WORK_DIR"
[ "$(stat -c %u:%g "$HOME_DIR")" = "$HOST_USER_UID:$HOST_USER_GID" ] || chown "$HOST_USER_UID:$HOST_USER_GID" "$HOME_DIR"
[ "$(stat -c %u "$WORK_DIR")" = "0" ] && chown "$HOST_USER_UID:$HOST_USER_GID" "$WORK_DIR" || true

run_as_user "$@"
