#compdef vlink-bench bench

# VLink zsh completion for vlink-bench.

typeset -ga _vlink_bench_suites=(throughput latency topology fanout scale serialization backpressure bp)
typeset -ga _vlink_bench_modes=(local-direct direct local-loop local loop process)
typeset -ga _vlink_bench_topologies=(1:1 1x1 one-to-one 1:n 1xn fanout one-to-many n:1 nx1 many-to-one n:n nxn many-to-many)
typeset -ga _vlink_bench_patterns=(max unlimited fixed rate burst)
typeset -ga _vlink_bench_payloads=(bytes raw string text rawdata zerocopy)
typeset -ga _vlink_bench_qos_profiles=(default event method field sensor parameter service clock static light poor better best large)
typeset -ga _vlink_bench_reports=(html terminal term tty csv json both)

_vlink-bench_suites() {
    _vlink_zsh_csv_values "${_vlink_bench_suites[@]}"
}

_vlink-bench_modes() {
    _vlink_zsh_csv_values "${_vlink_bench_modes[@]}"
}

_vlink-bench_topologies() {
    _vlink_zsh_csv_values "${_vlink_bench_topologies[@]}"
}

_vlink-bench_patterns() {
    _vlink_zsh_csv_values "${_vlink_bench_patterns[@]}"
}

_vlink-bench_payloads() {
    _vlink_zsh_csv_values "${_vlink_bench_payloads[@]}"
}

_vlink-bench_qos_profiles() {
    local -a qos_profiles=("${_vlink_bench_qos_profiles[@]}")
    local name

    while IFS= read -r name; do
        [[ -n "$name" && ${qos_profiles[(Ie)$name]} -eq 0 ]] && qos_profiles+=("$name")
    done < <(_vlink_qos_names_from_env)

    _vlink_zsh_csv_values "${qos_profiles[@]}"
}

_vlink-bench_reports() {
    _vlink_zsh_csv_values "${_vlink_bench_reports[@]}"
}

_vlink-bench_complete_run_multi() {
    case "$1" in
        -u|--url) _vlink_zsh_complete_url; return 0 ;;
        -s|--suite) _vlink-bench_suites; return 0 ;;
        -m|--mode) _vlink-bench_modes; return 0 ;;
        -t|--topology) _vlink-bench_topologies; return 0 ;;
        --pattern) _vlink-bench_patterns; return 0 ;;
        -k|--payload) _vlink-bench_payloads; return 0 ;;
        -q|--qos) _vlink-bench_qos_profiles; return 0 ;;
        --report) _vlink-bench_reports; return 0 ;;
        --property|--pub-property|--sub-property|--size|--latency-size|--topology-size|-r|--rate|-f|--fanout|\
        --publishers|--burst) return 0 ;;
    esac

    return 1
}

_vlink-bench_complete_worker_multi() {
    case "$1" in
        --property|--pub-property|--sub-property) return 0 ;;
    esac

    return 1
}

_vlink-bench_positional_count() {
    local subcommand="$1"
    local count=0 expect="" token seen_subcommand=0 index

    for ((index=2; index<CURRENT; ++index)); do
        token="${words[index]}"
        if (( seen_subcommand == 0 )); then
            [[ "$token" == "$subcommand" ]] && seen_subcommand=1
            continue
        fi

        case "$expect" in
            multi)
                if [[ "$token" == -* ]]; then
                    expect=""
                else
                    continue
                fi
                ;;
            single)
                expect=""
                continue
                ;;
        esac

        case "$subcommand:$token" in
            pub:--property|pub:--pub-property|pub:--sub-property|sub:--property|sub:--pub-property|sub:--sub-property)
                expect="multi"
                continue
                ;;
            pub:-k|pub:--payload|pub:-q|pub:--qos|pub:--pattern|pub:--size|pub:-r|pub:--rate|pub:--burst|\
            pub:--warmup|pub:--duration|pub:--drain|pub:--publisher-id|pub:-o|pub:--output|\
            sub:-k|sub:--payload|sub:-q|sub:--qos|sub:--pattern|sub:--size|sub:-r|sub:--rate|sub:--burst|\
            sub:--warmup|sub:--duration|sub:--drain|sub:--publisher-id|sub:--subscriber-sleep-us|sub:-o|sub:--output)
                expect="single"
                continue
                ;;
            -*)
                continue
                ;;
            *)
                ((count++))
                ;;
        esac
    done

    print -r -- "$count"
}

_vlink-bench_run() {
    local cur="${words[CURRENT]}"
    local last_option=""

    _vlink-bench_complete_run_multi "${words[CURRENT-1]}" && return

    if [[ "$cur" != -* ]]; then
        last_option=$(_vlink_zsh_last_option)
        _vlink-bench_complete_run_multi "$last_option" && return
    fi

    _arguments -s \
        '(-p --preset)'{-p,--preset}'=[Scenario preset]:preset:(showcase quick full)' \
        '*'{-u,--url}'=[Benchmark URL (repeatable)]:url:_vlink_zsh_complete_url' \
        '*'{-s,--suite}'=[Suite list (repeatable)]:suite:_vlink-bench_suites' \
        '*'{-m,--mode}'=[Mode list (repeatable)]:mode:_vlink-bench_modes' \
        '*'{-t,--topology}'=[Topology list (repeatable)]:topology:_vlink-bench_topologies' \
        '*--pattern=[Rate pattern list (repeatable)]:pattern:_vlink-bench_patterns' \
        '*'{-k,--payload}'=[Payload list (repeatable)]:payload:_vlink-bench_payloads' \
        '*'{-q,--qos}'=[QoS profile list (repeatable)]:qos:_vlink-bench_qos_profiles' \
        '*--property=[Node property (repeatable)]:property:' \
        '*--pub-property=[Publisher property (repeatable)]:property:' \
        '*--sub-property=[Subscriber property (repeatable)]:property:' \
        '*--size=[Payload size list (repeatable)]:size:' \
        '*--latency-size=[Latency payload size list (repeatable)]:size:' \
        '*--topology-size=[Topology payload size list (repeatable)]:size:' \
        '*'{-r,--rate}'=[Rate list (repeatable)]:rate:' \
        '*'{-f,--fanout}'=[Fanout subscriber count list (repeatable)]:count:' \
        '*--publishers=[Publisher count list (repeatable)]:count:' \
        '*--burst=[Burst message count list (repeatable)]:count:' \
        '--warmup=[Warmup duration in milliseconds]:ms:' \
        '--duration=[Run duration in milliseconds]:ms:' \
        '--drain=[Drain duration in milliseconds]:ms:' \
        '--serialization-duration=[Serialization suite duration in milliseconds]:ms:' \
        '--repeat=[Repeat count per scenario]:count:' \
        '(-o --output)'{-o,--output}'=[Output file prefix]:path:_files' \
        '*--report=[Output targets (repeatable)]:report:_vlink-bench_reports' \
        '--no-pager[Disable terminal pager]' \
        '--verbose[Verbose progress output]' \
        '(-h --help)'{-h,--help}'[Show help]' \
        '(-v --version)'{-v,--version}'[Show version]'
}

_vlink-bench_plot() {
    local cur="${words[CURRENT]}"
    local last_option=""

    if [[ "$cur" != -* ]]; then
        last_option=$(_vlink_zsh_last_option)
        [[ "$last_option" == "--report" ]] && _vlink-bench_reports && return
    fi

    _arguments -s \
        '(-o --output)'{-o,--output}'=[Output file prefix]:path:_files' \
        '*--report=[Output targets (repeatable)]:report:_vlink-bench_reports' \
        '--no-pager[Disable terminal pager]' \
        '(-h --help)'{-h,--help}'[Show help]' \
        '(-v --version)'{-v,--version}'[Show version]' \
        ':input bench json:_files -g "*.json"'
}

_vlink-bench_pub() {
    local cur="${words[CURRENT]}"
    local last_option=""
    local positional_count=0

    _vlink-bench_complete_worker_multi "${words[CURRENT-1]}" && return

    if [[ "$cur" != -* ]]; then
        last_option=$(_vlink_zsh_last_option)
        positional_count=$(_vlink-bench_positional_count pub)
        if _vlink-bench_complete_worker_multi "$last_option"; then
            return
        fi
        (( positional_count == 0 )) && _vlink_zsh_complete_url && return
    fi

    _arguments -s \
        '(-k --payload)'{-k,--payload}'=[Payload kind]:payload:(bytes raw string text rawdata zerocopy)' \
        '(-q --qos)'{-q,--qos}'=[QoS profile]:qos:(default event method field sensor parameter service clock static light poor better best large)' \
        '--pattern=[Rate pattern]:pattern:(max unlimited fixed rate burst)' \
        '*--property=[Node property]:property:' \
        '*--pub-property=[Publisher property]:property:' \
        '*--sub-property=[Subscriber property]:property:' \
        '--size=[Payload size in bytes]:size:' \
        '(-r --rate)'{-r,--rate}'=[Publish rate in Hz]:rate:' \
        '--burst=[Messages sent in each burst]:count:' \
        '--warmup=[Warmup duration in milliseconds]:ms:' \
        '--duration=[Run duration in milliseconds]:ms:' \
        '--drain=[Drain duration in milliseconds]:ms:' \
        '--publisher-id=[Publisher worker index]:id:' \
        '--wait-start[Wait for parent GO signal]' \
        '(-o --output)'{-o,--output}'=[Worker result json path]:path:_files' \
        '--verbose[Verbose worker output]' \
        '(-h --help)'{-h,--help}'[Show help]' \
        '(-v --version)'{-v,--version}'[Show version]' \
        ':url:_vlink_zsh_complete_url'
}

_vlink-bench_sub() {
    local cur="${words[CURRENT]}"
    local last_option=""
    local positional_count=0

    _vlink-bench_complete_worker_multi "${words[CURRENT-1]}" && return

    if [[ "$cur" != -* ]]; then
        last_option=$(_vlink_zsh_last_option)
        positional_count=$(_vlink-bench_positional_count sub)
        if _vlink-bench_complete_worker_multi "$last_option"; then
            return
        fi
        (( positional_count == 0 )) && _vlink_zsh_complete_url && return
    fi

    _arguments -s \
        '(-k --payload)'{-k,--payload}'=[Payload kind]:payload:(bytes raw string text rawdata zerocopy)' \
        '(-q --qos)'{-q,--qos}'=[QoS profile]:qos:(default event method field sensor parameter service clock static light poor better best large)' \
        '--pattern=[Rate pattern]:pattern:(max unlimited fixed rate burst)' \
        '*--property=[Node property]:property:' \
        '*--pub-property=[Publisher property]:property:' \
        '*--sub-property=[Subscriber property]:property:' \
        '--size=[Payload size in bytes]:size:' \
        '(-r --rate)'{-r,--rate}'=[Publisher cadence in Hz forwarded for worker metadata]:rate:' \
        '--burst=[Burst count forwarded from paired publisher]:count:' \
        '--warmup=[Warmup duration in milliseconds]:ms:' \
        '--duration=[Run duration in milliseconds]:ms:' \
        '--drain=[Drain duration in milliseconds]:ms:' \
        '--latency[Enable latency sampling]' \
        '--publisher-id=[Publisher worker index]:id:' \
        '--subscriber-sleep-us=[Microseconds to sleep per received message]:us:' \
        '--wait-start[Wait for parent GO signal]' \
        '(-o --output)'{-o,--output}'=[Worker result json path]:path:_files' \
        '--verbose[Verbose worker output]' \
        '(-h --help)'{-h,--help}'[Show help]' \
        '(-v --version)'{-v,--version}'[Show version]' \
        ':url:_vlink_zsh_complete_url'
}

_vlink-bench() {
    local curcontext="$curcontext" state line
    local -a subcommands=(
        'run:Run benchmark suites'
        'plot:Render report from bench json'
        'pub:Run publisher worker'
        'sub:Run subscriber worker'
    )

    _arguments -C \
        '(-h --help)'{-h,--help}'[Show help]' \
        '(-v --version)'{-v,--version}'[Show version]' \
        '1: :->cmd' \
        '*:: :->args'

    case "$state" in
        cmd)
            _describe -t commands 'vlink-bench command' subcommands
            ;;
        args)
            case "$line[1]" in
                run) _vlink-bench_run ;;
                plot) _vlink-bench_plot ;;
                pub) _vlink-bench_pub ;;
                sub) _vlink-bench_sub ;;
            esac
            ;;
    esac
}

if (( $+functions[compdef] )); then
    compdef _vlink-bench vlink-bench bench
fi
