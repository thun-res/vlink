# VLink bash completion for vlink-launcher.

_vlink_launcher() {
    local cur="${COMP_WORDS[COMP_CWORD]}"
    local prev="${COMP_WORDS[COMP_CWORD-1]}"
    local subcmd="${COMP_WORDS[1]:-}"
    local ctl_action="${COMP_WORDS[2]:-}"

    COMPREPLY=()

    if [[ ${COMP_CWORD} -eq 1 ]]; then
        _vlink_bash_complete_words "run validate graph inspect logs plugin init top ctl completion -h --help -v --version" "$cur"
        return
    fi

    case "$prev" in
        --scheme)
            _vlink_bash_complete_words "dds ddsc ddsr zenoh intra" "$cur"
            return
            ;;
        --format)
            _vlink_bash_complete_words "ascii dot mermaid" "$cur"
            return
            ;;
        --template)
            _vlink_bash_complete_words "minimal component systemd" "$cur"
            return
            ;;
        --log_level)
            _vlink_bash_complete_words "trace debug info warn error fatal off" "$cur"
            return
            ;;
        --level)
            _vlink_bash_complete_words "trace debug info warn error fatal" "$cur"
            return
            ;;
        --log_plugin)
            _vlink_bash_complete_words "logd" "$cur"
            return
            ;;
        --type)
            _vlink_bash_complete_words "component process container task proxy" "$cur"
            return
            ;;
        --state)
            _vlink_bash_complete_words "pending starting running ready inactive stopping stopped failed restarting skipped" "$cur"
            return
            ;;
        --param_type)
            _vlink_bash_complete_words "auto bool int double string bool_list int_list double_list string_list" "$cur"
            return
            ;;
        --path)
            _vlink_bash_complete_files_ext "$cur" "vcap|vcapx|vdb|vdbx"
            return
            ;;
        --log_dir|--record_dir)
            _vlink_bash_complete_dirs "$cur"
            return
            ;;
        --out|-o)
            _vlink_bash_complete_files "$cur"
            return
            ;;
    esac

    case "$subcmd" in
        run)
            if [[ "$cur" == -* ]]; then
                _vlink_bash_complete_words "--arg --dry_run --show_args --serial --parallel --only --skip --debug_unit \
--no_respawn --pause_on_fail --log_level --log_plugin --no_control --plain --log_dir --record_dir \
--scheme --namespace --domain --key --bind_ip --native -h --help" "$cur"
            else
                _vlink_bash_complete_files_ext "$cur" "toml"
            fi
            ;;
        validate)
            if [[ "$cur" == -* ]]; then
                _vlink_bash_complete_words "--arg --check_programs --json -h --help" "$cur"
            else
                _vlink_bash_complete_files_ext "$cur" "toml"
            fi
            ;;
        graph)
            if [[ "$cur" == -* ]]; then
                _vlink_bash_complete_words "--format --out -o --arg -h --help" "$cur"
            else
                _vlink_bash_complete_files_ext "$cur" "toml"
            fi
            ;;
        inspect)
            if [[ "$cur" == -* ]]; then
                _vlink_bash_complete_words "--show_args --json --arg -h --help" "$cur"
            else
                _vlink_bash_complete_files_ext "$cur" "toml"
            fi
            ;;
        plugin)
            if [[ "$cur" == -* ]]; then
                _vlink_bash_complete_words "--json -h --help" "$cur"
            else
                _vlink_bash_complete_files_ext "$cur" "so|dylib|dll"
            fi
            ;;
        init)
            _vlink_bash_complete_words "--template --out -o -h --help" "$cur"
            ;;
        top)
            _vlink_bash_complete_words "--scheme --namespace --domain --key --bind_ip --native -h --help" "$cur"
            ;;
        logs)
            if [[ "$cur" == -* ]]; then
                _vlink_bash_complete_words "--unit -u --grep --lines -n -h --help" "$cur"
            else
                _vlink_bash_complete_dirs "$cur"
            fi
            ;;
        completion)
            _vlink_bash_complete_words "bash zsh" "$cur"
            ;;
        ctl)
            if [[ ${COMP_CWORD} -eq 2 ]]; then
                _vlink_bash_complete_words "status list-units list-dependencies blame critical-chain cat \
start stop restart reload activate deactivate reset-failed isolate resume load unload \
params set-param record events logs shutdown" "$cur"
            elif [[ "$ctl_action" == "record" && ${COMP_CWORD} -eq 3 && "$cur" != -* ]]; then
                _vlink_bash_complete_words "start stop" "$cur"
            else
                _vlink_bash_complete_words "--unit -u --type --state --group --failed --reverse --watch -w --json \
--lines -n --follow -f --level --grep --stderr --lib --class --scheduler --compute_pool \
--container --worker --priority --legacy --param --url --input --output \
--param_key --param_val --param_type --path --stop --timeout \
--scheme --namespace --domain --key --bind_ip --native -h --help" "$cur"
            fi
            ;;
    esac
}

_vlink_bash_register_completion _vlink_launcher vlink-launcher launcher
