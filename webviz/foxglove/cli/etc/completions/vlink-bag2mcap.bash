# VLink bash completion for vlink-bag2mcap.

_vlink_bag2mcap() {
    local cur="${COMP_WORDS[COMP_CWORD]}"
    local prev="${COMP_WORDS[COMP_CWORD-1]}"
    local last_option=""

    COMPREPLY=()

    case "$prev" in
        -o|--output)
            _vlink_bash_complete_files_ext "$cur" "mcap"
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
        --compression)
            _vlink_bash_complete_words "none lz4 zstd" "$cur"
            return
            ;;
        --convert_plugin_config)
            return
            ;;
    esac

    if [[ "$cur" != -* ]]; then
        last_option=$(_vlink_bash_find_last_option)
        [[ "$last_option" == "--vlink_msgs" ]] && _vlink_bash_complete_files_ext "$cur" "json" && return
    fi

    if [[ "$cur" == -* ]]; then
        _vlink_bash_complete_words "-o --output --proto_dir --fbs_dir --schema_plugin \
--convert_plugin --convert_plugin_config --vlink_msgs --compression -h --help -v --version" "$cur"
        return
    fi

    _vlink_bash_complete_files_ext "$cur" "$_vlink_bash_play_ext"
}

complete -F _vlink_bag2mcap vlink-bag2mcap bag2mcap
