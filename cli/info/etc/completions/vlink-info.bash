# VLink bash completion for vlink-info.

_vlink_info() {
    local cur="${COMP_WORDS[COMP_CWORD]}"

    COMPREPLY=()
    _vlink_bash_complete_words "-l --list_options -h --help -v --version" "$cur"
}

complete -F _vlink_info vlink-info info
