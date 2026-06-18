# VLink bash completion for vlink-bag2rrd.

_vlink_bag2rrd() {
    local cur="${COMP_WORDS[COMP_CWORD]}"
    local prev="${COMP_WORDS[COMP_CWORD-1]}"
    local last_option=""

    COMPREPLY=()

    case "$prev" in
        -o|--output)
            _vlink_bash_complete_files_ext "$cur" "rrd"
            return
            ;;
        --proto_dir|--fbs_dir)
            _vlink_bash_complete_dirs "$cur"
            return
            ;;
        --schema_plugin|--convert_plugin)
            _vlink_bash_complete_files_ext "$cur" "$_vlink_bash_plugin_ext"
            return
            ;;
        --vlink_msgs)
            _vlink_bash_complete_files_ext "$cur" "json"
            return
            ;;
        --name|--convert_plugin_config|--sequence_timeline|--time_timeline|--timestamp_timeline)
            return
            ;;
    esac

    if [[ "$cur" != -* ]]; then
        last_option=$(_vlink_bash_find_last_option)
        [[ "$last_option" == "--vlink_msgs" ]] && _vlink_bash_complete_files_ext "$cur" "json" && return
    fi

    if [[ "$cur" == -* ]]; then
        _vlink_bash_complete_words "-o --output --proto_dir --fbs_dir --schema_plugin \
--convert_plugin --convert_plugin_config --vlink_msgs --name --sequence_timeline \
--time_timeline --timestamp_timeline --disable_time_timeline --disable_sequence_timeline \
--disable_timestamp_timeline -h --help -v --version" "$cur"
        return
    fi

    _vlink_bash_complete_files_ext "$cur" "$_vlink_bash_play_ext"
}

complete -F _vlink_bag2rrd vlink-bag2rrd bag2rrd
