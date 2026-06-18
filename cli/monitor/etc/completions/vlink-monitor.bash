# VLink bash completion for vlink-monitor.

_vlink_monitor() {
    local cur="${COMP_WORDS[COMP_CWORD]}"
    local prev="${COMP_WORDS[COMP_CWORD-1]}"
    local last_option=""

    COMPREPLY=()

    case "$prev" in
        -u|--urls)
            _vlink_bash_complete_url "$cur"
            return
            ;;
        -d|--proto_dir|-f|--fbs_dir)
            _vlink_bash_complete_dirs "$cur"
            return
            ;;
        -i|--filter|-g|--proto_args|--rows|--columns|--chart_width|--process_width)
            return
            ;;
    esac

    if [[ "$cur" != -* ]]; then
        last_option=$(_vlink_bash_find_last_option)
        if [[ "$last_option" == "-u" || "$last_option" == "--urls" ]]; then
            _vlink_bash_complete_url "$cur"
            return
        fi
    fi

    _vlink_bash_complete_words "-u --urls -i --filter -b --blob -k --black -n --native \
-t --node_count -l --detail -o --observe_all -e --profiler -s --ser -a --active \
-y --pubsub -p --process -c --chart -x --preset -g --proto_args -d --proto_dir \
-f --fbs_dir --plain --dot --rows --columns --chart_width --process_width \
-h --help -v --version" "$cur"
}

complete -F _vlink_monitor vlink-monitor monitor
