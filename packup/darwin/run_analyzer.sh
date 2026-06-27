#!/usr/bin/env bash

VLINK_BIN_DIR="$(cd "$(dirname "${BASH_SOURCE:-$0}")" && pwd)"
VLINK_ROOT_DIR="$(cd "$VLINK_BIN_DIR/.." && pwd)"

cd "$VLINK_ROOT_DIR"

export OSG_LIBRARY_PATH="$VLINK_ROOT_DIR/lib"/osgPlugins-3.*
export LD_LIBRARY_PATH="$VLINK_ROOT_DIR/lib:$LD_LIBRARY_PATH"
export DYLD_LIBRARY_PATH="$VLINK_ROOT_DIR/lib:$DYLD_LIBRARY_PATH"
export LC_RPATH="$VLINK_ROOT_DIR/lib:$LC_RPATH"

"$VLINK_BIN_DIR/vlink-analyzer"
