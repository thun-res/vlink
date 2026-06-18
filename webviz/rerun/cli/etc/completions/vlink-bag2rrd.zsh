#compdef vlink-bag2rrd bag2rrd

# VLink zsh completion for vlink-bag2rrd.

_vlink-bag2rrd_complete_multi_json() {
    case "$1" in
        --vlink_msgs)
            _vlink_zsh_complete_json_file
            return 0
            ;;
    esac

    return 1
}

_vlink-bag2rrd() {
    local cur="${words[CURRENT]}"
    local last_option=""

    _vlink-bag2rrd_complete_multi_json "${words[CURRENT-1]}" && return

    if [[ "$cur" != -* ]]; then
        last_option=$(_vlink_zsh_last_option)
        _vlink-bag2rrd_complete_multi_json "$last_option" && return
    fi

    _arguments -s \
        '(-o --output)'{-o,--output}'=[Output RRD file path]:file:_files -g "*.rrd"' \
        '--proto_dir=[Directory containing VLink .proto files for dynamic parsing]:dir:_files -/' \
        '--fbs_dir=[Directory containing VLink .fbs files for dynamic FlatBuffers parsing]:dir:_files -/' \
        '--schema_plugin=[Path to schema plugin shared library (imports protobuf/flatbuffers schemas; alternative to --proto_dir/--fbs_dir)]:plugin:_vlink_zsh_complete_plugin_file' \
        '--convert_plugin=[Path to message conversion plugin shared library]:plugin:_vlink_zsh_complete_plugin_file' \
        '--convert_plugin_config=[Configuration string for the conversion plugin]:cfg:' \
        '*--vlink_msgs=[Path to a vlink_msgs mapping JSON file (repeatable)]:file:_files -g "*.json"' \
        '--name=[Rerun application ID]:name:' \
        '--sequence_timeline=[Sequence timeline name]:name:' \
        '--time_timeline=[Relative time timeline name for bag playback]:name:' \
        '--timestamp_timeline=[Message timestamp timeline name]:name:' \
        '--disable_time_timeline[Disable relative bag time timeline logging]' \
        '--disable_sequence_timeline[Disable sequence timeline logging]' \
        '--disable_timestamp_timeline[Disable timestamp timeline logging]' \
        '(-h --help)'{-h,--help}'[Show help]' \
        '(-v --version)'{-v,--version}'[Show version]' \
        ':input bag:_vlink_zsh_complete_play_file'
}

if (( $+functions[compdef] )); then
    compdef _vlink-bag2rrd vlink-bag2rrd bag2rrd
fi
