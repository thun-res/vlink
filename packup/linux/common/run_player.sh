#!/usr/bin/env bash

VLINK_BIN_DIR="$(cd "$(dirname "${BASH_SOURCE:-$0}")" && pwd)"
VLINK_ROOT_DIR="$(cd "$VLINK_BIN_DIR/.." && pwd)"

cd "$VLINK_ROOT_DIR"

export OSG_LIBRARY_PATH="$VLINK_ROOT_DIR/lib"/osgPlugins-3.*
export LD_LIBRARY_PATH="$VLINK_ROOT_DIR/lib:$LD_LIBRARY_PATH"
if [ "$XDG_SESSION_TYPE" = "wayland" ]; then
    export QT_QPA_PLATFORM="xcb;wayland"
else
    export QT_QPA_PLATFORM="xcb"
fi

"$VLINK_BIN_DIR/vlink-player" "$@"
