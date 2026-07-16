#compdef vlink-trigger trigger

# VLink zsh completion for vlink-trigger.

_vlink-trigger_daemon() {
    _arguments -s \
        '(-c --config)'{-c,--config}'=[Config json path]:config:_files -g "*.json"' \
        '(-n --native)'{-n,--native}'[Native mode: local-host discovery + dds.ip=127.0.0.1 unless configured]' \
        '--bag_plugin=[Bag reorder plugin library loaded and bound by the CLI host]:plugin:' \
        '--trigger_plugin=[Trigger lifecycle plugin library name]:plugin:' \
        '--trigger_plugin_config=[Opaque trigger plugin configuration]:cfg:' \
        '(-h --help)'{-h,--help}'[Show help]'
}

_vlink-trigger_complete_url_list() {
    case "$1" in
        -u|--urls)
            _vlink_zsh_complete_url
            return 0
            ;;
    esac

    return 1
}

_vlink-trigger_dump() {
    local cur="${words[CURRENT]}"
    local last_option=""

    _vlink-trigger_complete_url_list "${words[CURRENT-1]}" && return

    if [[ "$cur" != -* ]]; then
        last_option=$(_vlink_zsh_last_option)
        _vlink-trigger_complete_url_list "$last_option" && return
    fi

    _arguments -s \
        '(-m --method_url)'{-m,--method_url}'=[Daemon control-plane URL]:url:_vlink_zsh_complete_url' \
        '(-o --out_file)'{-o,--out_file}'=[Output file path]:file:_files' \
        '(-r --reason)'{-r,--reason}'=[Trigger reason]:reason:' \
        '(-n --name)'{-n,--name}'=[Output file name hint]:name:' \
        '--pre=[Pre window ms (shrink only)]:pre:' \
        '--post=[Post window ms (shrink only)]:post:' \
        '*'{-u,--urls}'=[Filter urls, empty is all]:url:_vlink_zsh_complete_url' \
        '(-i --filter)'{-i,--filter}'=[URL keyword filter, comma-separated or quoted space-separated]:filter:' \
        '(-k --black)'{-k,--black}'[Blacklist mode for --filter]' \
        '(-h --help)'{-h,--help}'[Show help]'
}

_vlink-trigger() {
    local curcontext="$curcontext" state line
    local -a subcommands=(
        'daemon:Run the trigger recorder daemon'
        'dump:Trigger a dump on a running daemon'
    )

    _arguments -C \
        '(-h --help)'{-h,--help}'[Show help]' \
        '(-v --version)'{-v,--version}'[Show version]' \
        '1: :->cmd' \
        '*:: :->args'

    case "$state" in
        cmd)
            _describe -t commands 'vlink-trigger command' subcommands
            ;;
        args)
            case "$line[1]" in
                daemon) _vlink-trigger_daemon ;;
                dump) _vlink-trigger_dump ;;
            esac
            ;;
    esac
}

if (( $+functions[compdef] )); then
    _vlink_zsh_register_completion _vlink-trigger vlink-trigger trigger
fi
