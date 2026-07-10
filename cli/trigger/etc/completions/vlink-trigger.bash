# VLink bash completion for vlink-trigger.

_vlink_trigger() {
    local cur prev
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"

    local subcommand="" index
    for ((index=1; index<COMP_CWORD; index++)); do
        case "${COMP_WORDS[index]}" in
            daemon|trigger)
                subcommand="${COMP_WORDS[index]}"
                break
                ;;
        esac
    done

    if [[ -z "$subcommand" ]]; then
        COMPREPLY=($(compgen -W "daemon trigger -h --help -v --version" -- "$cur"))
        return 0
    fi

    case "$subcommand" in
        daemon)
            case "$prev" in
                -c|--config)
                    COMPREPLY=($(compgen -f -X '!*.json' -- "$cur"))
                    return 0
                    ;;
            esac
            COMPREPLY=($(compgen -W "-c --config -n --native -h --help" -- "$cur"))
            ;;
        trigger)
            case "$prev" in
                -o|--out_file)
                    COMPREPLY=($(compgen -f -- "$cur"))
                    return 0
                    ;;
            esac
            COMPREPLY=($(compgen -W "-m --method_url -o --out_file -r --reason -n --name --pre --post -u --url -i --filter -k --black -h --help" -- "$cur"))
            ;;
    esac

    return 0
}

complete -F _vlink_trigger vlink-trigger
complete -F _vlink_trigger trigger
