#!/usr/bin/env bash

export VLINK_ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE:-$0}")" && pwd)"
export VLINK_ETC_DIR="$VLINK_ROOT_DIR/etc"
export VLINK_COMPLETIONS="$VLINK_ETC_DIR/vlink/vlink-completions.sh"

echo -e -n "\033[2J\033[H"
echo -e "Setup vlink runtime..."
echo -e -n "\033[1;32m"
echo -e "╔═════════════════════════════════════╗"
echo -e "║  _    __   __      _           __   ║"
echo -e "║ | |  / /  / /     (_) ____    / /__ ║"
echo -e "║ | | / /  / /     / / / __ \\  / //_/ ║"
echo -e "║ | |/ /  / /___  / / / / / / / ,<    ║"
echo -e "║ |___/  /_____/ /_/ /_/ /_/ /_/|_|   ║"
echo -e "║                                     ║"
echo -e "╚═════════════════════════════════════╝"
echo -e -n "\033[0m"

[ -f "$VLINK_ROOT_DIR/version.txt" ] && echo -e "Version: $(cat "$VLINK_ROOT_DIR/version.txt")"
VLINK_PROTO_DIR_CONFIG="$HOME/.vlink_proto_dir"
VLINK_FBS_DIR_CONFIG="$HOME/.vlink_fbs_dir"

if [ -f "$VLINK_PROTO_DIR_CONFIG" ]; then
    export VLINK_PROTO_DIR="$(cat "$VLINK_PROTO_DIR_CONFIG")"
    echo "VLINK_PROTO_DIR: $VLINK_PROTO_DIR"
fi

if [ -f "$VLINK_FBS_DIR_CONFIG" ]; then
    export VLINK_FBS_DIR="$(cat "$VLINK_FBS_DIR_CONFIG")"
    echo "VLINK_FBS_DIR: $VLINK_FBS_DIR"
fi

echo -e "Support commands: [proxy] [info] [monitor] [bag] [list] [eproto] [efbs] [dump] [check] [bench] [viewer] [player] [analyzer] [webviz]"
echo -e ""

[[ "$PATH" != *"$VLINK_ROOT_DIR/bin"* ]] && export PATH="$VLINK_ROOT_DIR/bin:$PATH"
[[ "$LD_LIBRARY_PATH" != *"$VLINK_ROOT_DIR/lib"* ]] && export LD_LIBRARY_PATH="$VLINK_ROOT_DIR/lib:$LD_LIBRARY_PATH"

export VLINK_DIR="$VLINK_ROOT_DIR"
export vlink_DIR="$VLINK_ROOT_DIR/lib/cmake/vlink"
[[ ";$CMAKE_PREFIX_PATH;" != *";$VLINK_ROOT_DIR;"* ]] && export CMAKE_PREFIX_PATH="$VLINK_ROOT_DIR${CMAKE_PREFIX_PATH:+;$CMAKE_PREFIX_PATH}"

if [ "$XDG_SESSION_TYPE" = "wayland" ]; then
    export QT_QPA_PLATFORM="wayland;xcb"
else
    export QT_QPA_PLATFORM="xcb"
fi
# export VLINK_PROTOC_PROGRAM="$VLINK_ROOT_DIR/bin/protoc"
# export VLINK_FLATC_PROGRAM="$VLINK_ROOT_DIR/bin/flatc"
[ -f "$VLINK_COMPLETIONS" ] && source "$VLINK_COMPLETIONS"

function kill_proxy() {
    killall -q -9 proxy
    killall -q -9 vlink-proxy
}
