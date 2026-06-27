#compdef vlink-rerun webviz_rerun

# VLink zsh completion for vlink-rerun.

_vlink-rerun_complete_multi_json() {
    case "$1" in
        --vlink_msgs)
            _vlink_zsh_complete_json_file
            return 0
            ;;
    esac

    return 1
}

_vlink-rerun() {
    local cur="${words[CURRENT]}"
    local last_option=""

    _vlink-rerun_complete_multi_json "${words[CURRENT-1]}" && return

    if [[ "$cur" != -* ]]; then
        last_option=$(_vlink_zsh_last_option)
        _vlink-rerun_complete_multi_json "$last_option" && return
    fi

    _arguments -s \
        '(-m --mode)'{-m,--mode}'=[Rerun mode]:mode:(spawn connect serve save)' \
        '(-a --address)'{-a,--address}'=[Rerun address]:addr:' \
        '--bind_ip=[Bind IP]:ip:' \
        '(-p --port)'{-p,--port}'=[Port]:port:' \
        '--save_path=[Save to .rrd]:file:_files' \
        '--recording_id=[Recording id]:id:' \
        '(-c --config)'{-c,--config}'=[JSON config]:file:_files -g "*.json"' \
        '--name=[Rerun server name]:name:' \
        '--proto_dir=[Directory containing VLink .proto files for dynamic parsing]:dir:_files -/' \
        '--fbs_dir=[Directory containing VLink .fbs files for dynamic FlatBuffers parsing]:dir:_files -/' \
        '--schema_plugin=[Schema plugin]:plugin:_vlink_zsh_complete_plugin_file' \
        '--convert_plugin=[Convert plugin]:plugin:_vlink_zsh_complete_plugin_file' \
        '--convert_plugin_config=[Convert plugin config]:cfg:' \
        '*--vlink_msgs=[Path(s) to vlink_msgs mapping files]:file:_files -g "*.json"' \
        '--spawn_memory_limit=[Spawn memory limit]:limit:' \
        '--spawn_server_memory_limit=[Spawn server memory limit]:limit:' \
        '--spawn_hide_welcome_screen[Hide welcome screen]' \
        '--spawn_no_detach[Do not detach spawned process]' \
        '--spawn_executable_name=[Spawn executable name]:name:' \
        '--spawn_executable_path=[Spawn executable path]:file:_files' \
        '--serve_memory_limit=[Serve memory limit]:limit:' \
        '--playback_behavior=[Serve playback behavior\: oldest_first or newest_first]:behavior:(oldest_first newest_first)' \
        '--sequence_timeline=[Sequence timeline name]:name:' \
        '--timestamp_timeline=[Timestamp timeline name]:name:' \
        '--disable_sequence_timeline[Disable sequence timeline]' \
        '--disable_timestamp_timeline[Disable timestamp timeline]' \
        '--allow_multiple[Allow multiple instances]' \
        '(-i --filter)'{-i,--filter}'=[Filter]:filter:' \
        '(-k --black)'{-k,--black}'[Blacklist mode]' \
        '--proxy_interface=[Proxy interface]:iface:(proxy_api proxy_server)' \
        '--proxy_role=[Proxy role]:role:(controller listener)' \
        '--proxy_domain_id=[Proxy domain id]:id:' \
        '--proxy_dds_impl=[Proxy DDS impl]:impl:(dds ddsc ddsr ddst)' \
        '--proxy_bind_ip=[Proxy bind IP]:ip:' \
        '--proxy_peer_ip=[Proxy peer IP]:ip:' \
        '--proxy_buf_size=[Proxy buffer size]:n:' \
        '--proxy_mtu_size=[Proxy MTU size]:n:' \
        '--proxy_native[Proxy native]' \
        '--proxy_tcp[Proxy TCP]' \
        '--proxy_key=[Proxy security key]:key:' \
        '--proxy_reliable[Proxy reliable]' \
        '--proxy_direct[Proxy direct]' \
        '--proxy_no_match_version[Proxy skip version match]' \
        '--proxy_data_callback_mode=[Proxy data callback mode]:mode:(direct queued)' \
        '--proxy_max_packet_size=[Proxy max packet size (MiB)]:n:' \
        '--proxy_use_iox[Proxy use iceoryx]' \
        '--proxy_iox_config=[Proxy iox config]:file:_files -g "*.toml"' \
        '--proxy_iox_strategy=[Proxy iox strategy]:n:(1 2 3)' \
        '--proxy_iox_monitoring=[Proxy iox monitoring]:mode:(on off)' \
        '(-h --help)'{-h,--help}'[Show help]' \
        '(-v --version)'{-v,--version}'[Show version]'
}

if (( $+functions[compdef] )); then
    compdef _vlink-rerun vlink-rerun webviz_rerun
fi
