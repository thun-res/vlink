#compdef vlink-monitor monitor

# VLink zsh completion for vlink-monitor.

_vlink-monitor_complete_multi() {
    case "$1" in
        -u|--urls)
            _vlink_zsh_complete_url
            return 0
            ;;
    esac

    return 1
}

_vlink-monitor() {
    local cur="${words[CURRENT]}"
    local last_option=""

    _vlink-monitor_complete_multi "${words[CURRENT-1]}" && return

    if [[ "$cur" != -* ]]; then
        last_option=$(_vlink_zsh_last_option)
        _vlink-monitor_complete_multi "$last_option" && return
    fi

    _arguments -s \
        '*'{-u,--urls}'=[Bind urls (repeatable)]:url:_vlink_zsh_complete_url' \
        '(-i --filter)'{-i,--filter}'=[Filter]:filter:' \
        '(-b --blob)'{-b,--blob}'[Force blob output for Enter jump]' \
        '(-k --black)'{-k,--black}'[Blacklist mode]' \
        '(-n --native)'{-n,--native}'[Native mode]' \
        '(-t --node_count)'{-t,--node_count}'[Node count mode]' \
        '(-l --detail)'{-l,--detail}'[Detail mode (hot key l)]' \
        "(-o --observe_all)"{-o,--observe_all}"[Observe all mode (hot key O, same as '-o')]" \
        '(-e --profiler)'{-e,--profiler}'[Profiler mode (hot key e)]' \
        '(-s --ser)'{-s,--ser}'[Show ser (hot key s)]' \
        '(-a --active)'{-a,--active}'[Active-only (hot key a)]' \
        '(-y --pubsub)'{-y,--pubsub}'[Pub/Sub mode (hot key y)]' \
        "(-p --process)"{-p,--process}"[Show process panel (hot key P, same as '-p')]" \
        '(-c --chart)'{-c,--chart}'[Chart panel (hot key c)]' \
        "(-x --preset)"{-x,--preset}"[Preset mode (same as '-l -o -p -c')]" \
        '(-g --proto_args)'{-g,--proto_args}'=[Proto args]:args:' \
        '(-d --proto_dir)'{-d,--proto_dir}'=[Proto dir]:dir:_files -/' \
        '(-f --fbs_dir)'{-f,--fbs_dir}'=[FBS dir]:dir:_files -/' \
        '--plain[Plain text output]' \
        '--dot[Use chart dot to paint]' \
        '--rows=[Maximum rows (0 means automatic)]:rows:' \
        '--columns=[Maximum columns (0 means automatic)]:cols:' \
        '--chart_width=[Chart width (range 10-100)]:n:' \
        '--process_width=[Process width (range 20-100)]:n:' \
        '(-h --help)'{-h,--help}'[Show help]' \
        '(-v --version)'{-v,--version}'[Show version]'
}

if (( $+functions[compdef] )); then
    compdef _vlink-monitor vlink-monitor monitor
fi
