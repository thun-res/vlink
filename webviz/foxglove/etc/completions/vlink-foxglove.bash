# VLink bash completion for vlink-foxglove.

_vlink_foxglove() {
    local cur="${COMP_WORDS[COMP_CWORD]}"
    local prev="${COMP_WORDS[COMP_CWORD-1]}"
    local last_option=""

    COMPREPLY=()

    _vlink_bash_complete_webviz_proxy_prev "$prev" "$cur" && return

    case "$prev" in
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
        --vlink_msgs|--foxglove_msgs|--rpc_msgs)
            _vlink_bash_complete_files_ext "$cur" "json"
            return
            ;;
        --parameters_url)
            _vlink_bash_complete_url "$cur"
            return
            ;;
        -p|--port|-a|--address|--name|--convert_plugin_config|-i|--filter)
            return
            ;;
    esac

    if [[ "$cur" != -* ]]; then
        last_option=$(_vlink_bash_find_last_option)
        case "$last_option" in
            --vlink_msgs|--foxglove_msgs|--rpc_msgs)
                _vlink_bash_complete_files_ext "$cur" "json"
                return
                ;;
        esac
    fi

    _vlink_bash_complete_words "-p --port -a --address -c --config --name --proto_dir --fbs_dir \
--schema_plugin --convert_plugin --convert_plugin_config --vlink_msgs --foxglove_msgs --rpc_msgs \
--send_time --parameters_url -i --filter -k --black --allow_multiple \
$_vlink_bash_webviz_proxy_opts -h --help -v --version" "$cur"
}

complete -F _vlink_foxglove vlink-foxglove webviz_foxglove
