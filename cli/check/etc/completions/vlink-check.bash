# VLink bash completion for vlink-check.

_vlink_check() {
    local cur="${COMP_WORDS[COMP_CWORD]}"
    local prev="${COMP_WORDS[COMP_CWORD-1]}"
    local subcommands="diag env test"
    local subcommand

    COMPREPLY=()
    subcommand=$(_vlink_bash_find_subcommand "$subcommands")

    if [[ -z "$subcommand" ]]; then
        _vlink_bash_complete_words "$subcommands -h --help -v --version" "$cur"
        return
    fi

    # Options taking an argument: let the user type a free value.
    case "$prev" in
        -f|--filter|-p|--prefix)
            COMPREPLY=()
            return
            ;;
    esac

    case "$subcommand" in
        diag) _vlink_bash_complete_words "-a --all -s --summary -f --filter -h --help" "$cur" ;;
        env)  _vlink_bash_complete_words "-b --available -p --prefix -h --help" "$cur" ;;
        test) _vlink_bash_complete_words "-h --help" "$cur" ;;
    esac
}

complete -F _vlink_check vlink-check check
