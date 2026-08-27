#!/usr/bin/env bash
set -euo pipefail

compiler="${1:?Compiler is required}"
shift

source_file=""
next_is_source=0
for arg in "$@"; do
    if [ "$next_is_source" = "1" ]; then
        source_file="$arg"
        break
    fi
    if [ "$arg" = "-c" ]; then
        next_is_source=1
    fi
done

case "$source_file" in
    *.cc | *.cpp | *.cxx)
        exec cmake -E __run_co_compile \
            "--tidy=clang-tidy;--extra-arg-before=--driver-mode=g++" \
            "--source=$source_file" -- "$compiler" "$@"
        ;;
    *)
        exec "$compiler" "$@"
        ;;
esac
