#compdef vlink-trigger trigger

# VLink zsh completion for vlink-trigger.

_vlink-trigger_daemon() {
    _arguments -s \
        '(-c --config)'{-c,--config}'=[Config json path]:config:_files -g "*.json"' \
        '(-n --native)'{-n,--native}'[Native mode: local-host discovery + dds.ip=127.0.0.1]' \
        '(-h --help)'{-h,--help}'[Show help]'
}

_vlink-trigger_trigger() {
    _arguments -s \
        '(-m --method_url)'{-m,--method_url}'=[Daemon control-plane URL]:url:' \
        '(-o --out_file)'{-o,--out_file}'=[Output file path]:file:_files' \
        '(-r --reason)'{-r,--reason}'=[Trigger reason]:reason:' \
        '(-n --name)'{-n,--name}'=[Output file name hint]:name:' \
        '--pre=[Pre window ms (shrink only)]:pre:' \
        '--post=[Post window ms (shrink only)]:post:' \
        '*'{-u,--url}'=[Filter: dump only these exact urls]:url:' \
        '(-i --filter)'{-i,--filter}'=[Filter urls by space-separated substrings]:filter:' \
        '(-k --black)'{-k,--black}'[Blacklist mode for --filter]' \
        '(-h --help)'{-h,--help}'[Show help]'
}

_vlink-trigger() {
    local curcontext="$curcontext" state line
    local -a subcommands=(
        'daemon:Run the trigger recorder daemon'
        'trigger:Send a trigger to a running daemon'
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
                trigger) _vlink-trigger_trigger ;;
            esac
            ;;
    esac
}

if (( $+functions[compdef] )); then
    compdef _vlink-trigger vlink-trigger trigger
fi
