#compdef vlink-proxy proxy

# VLink zsh completion for vlink-proxy.

_vlink-proxy_complete_multi() {
    case "$1" in
        --runnable) return 0 ;;
    esac

    return 1
}

_vlink-proxy() {
    local cur="${words[CURRENT]}"
    local last_option=""

    _vlink-proxy_complete_multi "${words[CURRENT-1]}" && return

    if [[ "$cur" != -* ]]; then
        last_option=$(_vlink_zsh_last_option)
        _vlink-proxy_complete_multi "$last_option" && return
    fi

    _arguments -s \
        '(-a --async)'{-a,--async}'[Async mode]' \
        '(-r --reliable)'{-r,--reliable}'[Reliable mode]' \
        '(-t --tcp)'{-t,--tcp}'[TCP mode]' \
        '(-g --direct)'{-g,--direct}'[Direct mode]' \
        '(-d --domain_id)'{-d,--domain_id}'=[Domain id 0..255]:id:' \
        '(-k --key)'{-k,--key}'=[Security key]:key:' \
        '(-b --bind_ip)'{-b,--bind_ip}'=[Bind IP]:ip:' \
        '(-p --peer_ip)'{-p,--peer_ip}'=[Peer IP]:ip:' \
        '(-s --buf_size)'{-s,--buf_size}'=[DDS TX/RX buffer size]:size:' \
        '(-e --mtu_size)'{-e,--mtu_size}'=[DDS MTU size]:size:' \
        '(-n --native)'{-n,--native}'[Native mode]' \
        '(-x --max_packet_size)'{-x,--max_packet_size}'=[Max packet size (MB)]:size:' \
        '(-c --iox_config)'{-c,--iox_config}'=[Iceoryx config]:file:_files -g "*.toml"' \
        '(-l --iox_strategy)'{-l,--iox_strategy}'=[Iceoryx memory strategy]:level:(1 2 3)' \
        '(-m --iox_monitoring)'{-m,--iox_monitoring}'=[Iceoryx monitoring]:mode:(on off)' \
        '--dds_impl=[DDS implementation transport]:impl:(dds ddsc ddsr ddst)' \
        '*--runnable=[Load runnable plugins]:plugin:' \
        '(-h --help)'{-h,--help}'[Show help]' \
        '(-v --version)'{-v,--version}'[Show version]'
}

if (( $+functions[compdef] )); then
    compdef _vlink-proxy vlink-proxy proxy
fi
