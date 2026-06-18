# VLink bash completion for vlink-rerun.

_vlink_rerun() {
    local cur="${COMP_WORDS[COMP_CWORD]}"
    local prev="${COMP_WORDS[COMP_CWORD-1]}"
    local last_option=""

    COMPREPLY=()

    _vlink_bash_complete_webviz_proxy_prev "$prev" "$cur" && return

    case "$prev" in
        -m|--mode)
            _vlink_bash_complete_words "spawn connect serve save" "$cur"
            return
            ;;
        --save_path)
            _vlink_bash_complete_files "$cur"
            return
            ;;
        -c|--config)
            _vlink_bash_complete_files_ext "$cur" "json"
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
        --spawn_executable_path)
            _vlink_bash_complete_files "$cur"
            return
            ;;
        --playback_behavior)
            _vlink_bash_complete_words "oldest_first newest_first" "$cur"
            return
            ;;
        -a|--address|--bind_ip|-p|--port|--recording_id|--name|--convert_plugin_config|\
        --spawn_memory_limit|--spawn_server_memory_limit|--spawn_executable_name|\
        --serve_memory_limit|--sequence_timeline|--timestamp_timeline|-i|--filter)
            return
            ;;
    esac

    if [[ "$cur" != -* ]]; then
        last_option=$(_vlink_bash_find_last_option)
        [[ "$last_option" == "--vlink_msgs" ]] && _vlink_bash_complete_files_ext "$cur" "json" && return
    fi

    _vlink_bash_complete_words "-m --mode -a --address --bind_ip -p --port --save_path --recording_id \
-c --config --name --proto_dir --fbs_dir --schema_plugin --convert_plugin \
--convert_plugin_config --vlink_msgs --spawn_memory_limit --spawn_server_memory_limit \
--spawn_hide_welcome_screen --spawn_no_detach --spawn_executable_name --spawn_executable_path \
--serve_memory_limit --playback_behavior --sequence_timeline --timestamp_timeline \
--disable_sequence_timeline --disable_timestamp_timeline --allow_multiple \
-i --filter -k --black $_vlink_bash_webviz_proxy_opts -h --help -v --version" "$cur"
}

complete -F _vlink_rerun vlink-rerun webviz_rerun
