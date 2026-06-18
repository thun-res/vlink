# VLink bash completion for vlink-bench.

_vlink_bench_presets="showcase quick full"
_vlink_bench_suites="throughput latency topology fanout scale serialization backpressure bp"
_vlink_bench_modes="local-direct direct local-loop local loop process"
_vlink_bench_topologies="1:1 1x1 one-to-one 1:n 1xn fanout one-to-many n:1 nx1 many-to-one n:n nxn many-to-many"
_vlink_bench_patterns="max unlimited fixed rate burst"
_vlink_bench_payloads="bytes raw string text rawdata zerocopy"
_vlink_bench_reports="html terminal term tty csv json both"
_vlink_bench_qos="default event method field sensor parameter service clock static light poor better best large"

_vlink_bench_compgen_csv() {
    local values="$1"
    local cur="$2"
    local prefix="" tail="$cur"
    local candidate

    if [[ "$cur" == *,* ]]; then
        prefix="${cur%,*},"
        tail="${cur##*,}"
    fi

    COMPREPLY=()
    while IFS= read -r candidate; do
        COMPREPLY+=("${prefix}${candidate}")
    done < <(compgen -W "$values" -- "$tail")
}

_vlink_bench_complete_urls_csv() {
    local cur="$1"
    local urls="" scheme

    for scheme in $_vlink_bash_schemes; do
        urls+="${scheme}:// "
    done
    _vlink_bench_compgen_csv "$urls" "$cur"
    compopt -o nospace 2>/dev/null || true
}

_vlink_bench_complete_reports_csv() {
    _vlink_bench_compgen_csv "$1" "$2"
}

_vlink_bench_complete_qos_csv() {
    local values="$_vlink_bench_qos"
    local name

    while IFS= read -r name; do
        [[ -n "$name" && " $values " != *" $name "* ]] && values+=" $name"
    done < <(_vlink_qos_names_from_env)

    _vlink_bench_compgen_csv "$values" "$1"
}

_vlink_bench_complete_run_multi() {
    case "$1" in
        -u|--url) _vlink_bench_complete_urls_csv "$2"; return 0 ;;
        -s|--suite) _vlink_bench_compgen_csv "$_vlink_bench_suites" "$2"; return 0 ;;
        -m|--mode) _vlink_bench_compgen_csv "$_vlink_bench_modes" "$2"; return 0 ;;
        -t|--topology) _vlink_bench_compgen_csv "$_vlink_bench_topologies" "$2"; return 0 ;;
        --pattern) _vlink_bench_compgen_csv "$_vlink_bench_patterns" "$2"; return 0 ;;
        -k|--payload) _vlink_bench_compgen_csv "$_vlink_bench_payloads" "$2"; return 0 ;;
        -q|--qos) _vlink_bench_complete_qos_csv "$2"; return 0 ;;
        --report) _vlink_bench_complete_reports_csv "$_vlink_bench_reports" "$2"; return 0 ;;
        --property|--pub-property|--sub-property|--size|--latency-size|--topology-size|-r|--rate|-f|--fanout|\
        --publishers|--burst) return 0 ;;
    esac

    return 1
}

_vlink_bench_complete_worker_multi() {
    case "$1" in
        --property|--pub-property|--sub-property) return 0 ;;
    esac

    return 1
}

_vlink_bench_positional_count() {
    local subcommand="$1"
    local index count=0 expect="" seen_subcommand=0

    for ((index=1; index<COMP_CWORD; index++)); do
        local token="${COMP_WORDS[index]}"

        if (( seen_subcommand == 0 )); then
            if [[ "$token" == "$subcommand" ]]; then
                seen_subcommand=1
            fi
            continue
        fi

        case "$expect" in
            multi)
                if [[ "$token" == -* ]]; then
                    expect=""
                    ((index--))
                fi
                continue
                ;;
            single)
                expect=""
                continue
                ;;
        esac

        case "$subcommand:$token" in
            run:-u|run:--url|run:-s|run:--suite|run:-m|run:--mode|run:-t|run:--topology|run:--pattern|\
            run:-k|run:--payload|run:-q|run:--qos|run:--property|run:--pub-property|run:--sub-property|\
            run:--size|run:--latency-size|run:--topology-size|run:-r|run:--rate|run:-f|run:--fanout|\
            run:--publishers|run:--burst|run:--report|plot:--report|pub:--property|pub:--pub-property|pub:--sub-property|\
            sub:--property|sub:--pub-property|sub:--sub-property)
                expect="multi"
                continue
                ;;
            run:-p|run:--preset|run:--warmup|run:--duration|run:--drain|run:--serialization-duration|\
            run:--repeat|run:-o|run:--output|plot:-o|plot:--output|\
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

    printf '%s' "$count"
}

_vlink_bench() {
    local cur="${COMP_WORDS[COMP_CWORD]}"
    local prev="${COMP_WORDS[COMP_CWORD-1]}"
    local subcommands="run plot pub sub"
    local subcommand
    local last_option="" positional_count=0

    COMPREPLY=()
    subcommand=$(_vlink_bash_find_subcommand "$subcommands")

    if [[ -z "$subcommand" ]]; then
        _vlink_bash_complete_words "$subcommands -h --help -v --version" "$cur"
        return
    fi

    case "$subcommand" in
        run)
            case "$prev" in
                -p|--preset)
                    _vlink_bash_complete_words "$_vlink_bench_presets" "$cur"
                    return
                    ;;
                -u|--url|-s|--suite|-m|--mode|-t|--topology|--pattern|-k|--payload|-q|--qos|\
                --property|--pub-property|--sub-property|--size|--latency-size|--topology-size|-r|--rate|\
                -f|--fanout|--publishers|--burst)
                    _vlink_bench_complete_run_multi "$prev" "$cur" && return
                    ;;
                --warmup|--duration|--drain|--serialization-duration|--repeat)
                    return
                    ;;
                -o|--output)
                    _vlink_bash_complete_files "$cur"
                    return
                    ;;
                --report)
                    _vlink_bench_complete_reports_csv "$_vlink_bench_reports" "$cur"
                    return
                    ;;
            esac

            if [[ "$cur" != -* ]]; then
                last_option=$(_vlink_bash_find_last_option)
                _vlink_bench_complete_run_multi "$last_option" "$cur" && return
            fi

            _vlink_bash_complete_words "-p --preset -u --url -s --suite -m --mode -t --topology \
--pattern -k --payload -q --qos --property --pub-property --sub-property \
--size --latency-size --topology-size -r --rate -f --fanout --publishers \
--burst --warmup --duration --drain --serialization-duration --repeat \
-o --output --report --no-pager --verbose -h --help -v --version" "$cur"
            ;;
        plot)
            case "$prev" in
                -o|--output)
                    _vlink_bash_complete_files "$cur"
                    return
                    ;;
                --report)
                    _vlink_bench_complete_reports_csv "$_vlink_bench_reports" "$cur"
                    return
                    ;;
            esac

            if [[ "$cur" != -* ]]; then
                last_option=$(_vlink_bash_find_last_option)
                if [[ "$last_option" == "--report" ]]; then
                    _vlink_bench_complete_reports_csv "$_vlink_bench_reports" "$cur"
                    return
                fi
            fi

            if [[ "$cur" == -* ]]; then
                _vlink_bash_complete_words "-o --output --report --no-pager -h --help -v --version" "$cur"
                return
            fi

            positional_count=$(_vlink_bench_positional_count "$subcommand")
            (( positional_count == 0 )) && _vlink_bash_complete_files_ext "$cur" "json"
            ;;
        pub|sub)
            case "$prev" in
                -k|--payload)
                    _vlink_bash_complete_words "$_vlink_bench_payloads" "$cur"
                    return
                    ;;
                -q|--qos)
                    local qos_values="$_vlink_bench_qos" qos_name
                    while IFS= read -r qos_name; do
                        [[ -n "$qos_name" && " $qos_values " != *" $qos_name "* ]] && qos_values+=" $qos_name"
                    done < <(_vlink_qos_names_from_env)
                    _vlink_bash_complete_words "$qos_values" "$cur"
                    return
                    ;;
                --pattern)
                    _vlink_bash_complete_words "$_vlink_bench_patterns" "$cur"
                    return
                    ;;
                --property|--pub-property|--sub-property)
                    _vlink_bench_complete_worker_multi "$prev" "$cur" && return
                    ;;
                --size|-r|--rate|--burst|--warmup|--duration|--drain|--publisher-id|--subscriber-sleep-us)
                    return
                    ;;
                -o|--output)
                    _vlink_bash_complete_files "$cur"
                    return
                    ;;
            esac

            if [[ "$cur" != -* ]]; then
                last_option=$(_vlink_bash_find_last_option)
                positional_count=$(_vlink_bench_positional_count "$subcommand")
                if _vlink_bench_complete_worker_multi "$last_option" "$cur"; then
                    return
                fi
                if (( positional_count == 0 )); then
                    _vlink_bash_complete_url "$cur"
                    return
                fi
            fi

            if [[ "$cur" == -* ]]; then
                if [[ "$subcommand" == "pub" ]]; then
                    _vlink_bash_complete_words "-k --payload -q --qos --pattern --property --pub-property \
--sub-property --size -r --rate --burst --warmup --duration --drain \
--publisher-id --wait-start -o --output --verbose -h --help -v --version" "$cur"
                else
                    _vlink_bash_complete_words "-k --payload -q --qos --pattern --property --pub-property \
--sub-property --size -r --rate --burst --warmup --duration --drain \
--latency --publisher-id --subscriber-sleep-us --wait-start -o --output --verbose -h --help -v --version" "$cur"
                fi
            fi
            ;;
    esac
}

complete -F _vlink_bench vlink-bench bench
