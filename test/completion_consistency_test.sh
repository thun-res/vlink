#!/usr/bin/env bash

set -u

if (( $# < 4 )); then
    echo "Usage: $0 BINARY BASH_COMPLETION ZSH_COMPLETION COMMON_COMPLETION [SUBCOMMAND ...]" >&2
    exit 2
fi

binary="$1"
bash_completion="$2"
zsh_completion="$3"
common_completion="$4"
shift 4

status=0

check_help() {
    local context="$1"
    shift

    local help_output
    local option
    if ! help_output=$("$binary" "$@" --help 2>&1); then
        echo "Unable to read help for $context" >&2
        status=1
        return
    fi

    while IFS= read -r option; do
        if ! grep -Fq -- "$option" "$bash_completion" && ! grep -Fq -- "$option" "$common_completion"; then
            echo "$bash_completion: missing $option for $context" >&2
            status=1
        fi

        if ! grep -Fq -- "$option" "$zsh_completion"; then
            echo "$zsh_completion: missing $option for $context" >&2
            status=1
        fi
    done < <(
        grep -oE -- '(^|[[:space:]])-{1,2}[[:alnum:]][[:alnum:]_-]*' <<<"$help_output" |
            sed 's/^[[:space:]]*//' |
            grep -E '^--|^-[[:alpha:]]$' |
            sort -u
    )
}

check_help "$(basename "$binary")"

for subcommand in "$@"; do
    check_help "$(basename "$binary") $subcommand" "$subcommand"
done

exit "$status"
