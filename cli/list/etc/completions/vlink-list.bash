# VLink bash completion for vlink-list.

_vlink_list() {
    local cur="${COMP_WORDS[COMP_CWORD]}"
    local prev="${COMP_WORDS[COMP_CWORD-1]}"

    COMPREPLY=()

    case "$prev" in
        -m|--name|-p|--pid) return ;;
    esac

    _vlink_bash_complete_words "-n --native -m --name -p --pid -c --check_process_count -h --help -v --version" "$cur"
}

complete -F _vlink_list vlink-list list
