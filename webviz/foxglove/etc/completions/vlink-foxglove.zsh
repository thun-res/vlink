#compdef vlink-foxglove webviz_foxglove

# VLink zsh completion for vlink-foxglove.

_vlink-foxglove_complete_multi_json() {
    case "$1" in
        --vlink_msgs|--foxglove_msgs|--rpc_msgs)
            _vlink_zsh_complete_json_file
            return 0
            ;;
    esac

    return 1
}

_vlink-foxglove() {
    local cur="${words[CURRENT]}"
    local last_option=""

    _vlink-foxglove_complete_multi_json "${words[CURRENT-1]}" && return

    if [[ "$cur" != -* ]]; then
        last_option=$(_vlink_zsh_last_option)
        _vlink-foxglove_complete_multi_json "$last_option" && return
    fi

    _arguments -s \
        '(-p --port)'{-p,--port}'=[Listen port]:port:' \
        '(-a --address)'{-a,--address}'=[Bind address]:addr:' \
        '(-c --config)'{-c,--config}'=[JSON config]:file:_files -g "*.json"' \
        '--name=[Foxglove server name]:name:' \
        '--proto_dir=[Proto dir]:dir:_files -/' \
        '--fbs_dir=[FBS dir]:dir:_files -/' \
        '--schema_plugin=[Schema plugin]:plugin:_vlink_zsh_complete_plugin_file' \
        '--convert_plugin=[Convert plugin]:plugin:_vlink_zsh_complete_plugin_file' \
        '--convert_plugin_config=[Convert plugin config]:cfg:' \
        '*--vlink_msgs=[Path(s) to vlink_msgs mapping files]:file:_files -g "*.json"' \
        '*--foxglove_msgs=[Path(s) to foxglove_msgs publish route config files]:file:_files -g "*.json"' \
        '*--rpc_msgs=[Path(s) to Foxglove rpc service config files]:file:_files -g "*.json"' \
        '--send_time[Send time updates to frontend clients]' \
        '--parameters_url=[Parameters URL]:url:_vlink_zsh_complete_url' \
        '(-i --filter)'{-i,--filter}'=[Filter]:filter:' \
        '(-k --black)'{-k,--black}'[Blacklist mode]' \
        '--allow_multiple[Allow multiple instances]' \
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
    compdef _vlink-foxglove vlink-foxglove webviz_foxglove
fi
