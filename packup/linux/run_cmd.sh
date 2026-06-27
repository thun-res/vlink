#!/bin/bash

WORK_DIR="$(cd "$(dirname "${BASH_SOURCE:-$0}")" && pwd)"

bash --init-file <(echo "cd \"$(pwd)\" && source \"$WORK_DIR/../setup_runtime.sh\"")
