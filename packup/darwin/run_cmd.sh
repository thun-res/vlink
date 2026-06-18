#!/usr/bin/env bash

WORK_DIR="$(cd "$(dirname "${BASH_SOURCE:-$0}")" && pwd)"
INIT_SCRIPT="$(mktemp)"

cat <<EOF > "$INIT_SCRIPT"
source "$WORK_DIR/../setup_runtime.sh"
exec zsh
EOF

exec zsh "$INIT_SCRIPT"
