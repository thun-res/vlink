# VLink bash completion for vlink-proxy.

_vlink_proxy() {
    local cur="${COMP_WORDS[COMP_CWORD]}"
    local prev="${COMP_WORDS[COMP_CWORD-1]}"
    local last_option=""

    COMPREPLY=()

    case "$prev" in
        -m|--iox_monitoring)
            _vlink_bash_complete_words "on off" "$cur"
            return
            ;;
        --dds_impl)
            _vlink_bash_complete_words "dds ddsc ddsr ddst" "$cur"
            return
            ;;
        -l|--iox_strategy)
            _vlink_bash_complete_words "1 2 3" "$cur"
            return
            ;;
        -c|--iox_config)
            _vlink_bash_complete_files_ext "$cur" "toml"
            return
            ;;
        --runnable)
            return
            ;;
        -d|--domain_id|-k|--key|-b|--bind_ip|-p|--peer_ip|-s|--buf_size|-e|--mtu_size|-x|--max_packet_size)
            return
            ;;
    esac

    if [[ "$cur" != -* ]]; then
        last_option=$(_vlink_bash_find_last_option)
        [[ "$last_option" == "--runnable" ]] && return
    fi

    _vlink_bash_complete_words "-a --async -r --reliable -t --tcp -g --direct -d --domain_id \
-k --key -b --bind_ip -p --peer_ip -s --buf_size -e --mtu_size -n --native \
-x --max_packet_size -c --iox_config -l --iox_strategy -m --iox_monitoring \
--dds_impl --runnable -h --help -v --version" "$cur"
}

complete -F _vlink_proxy vlink-proxy proxy
