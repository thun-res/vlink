# VLink bash completion for vlink-launcher.

_vlink_launcher() {
    local cur="${COMP_WORDS[COMP_CWORD]}"
    local prev="${COMP_WORDS[COMP_CWORD-1]}"
    local subcmd="${COMP_WORDS[1]:-}"

    COMPREPLY=()

    if [[ ${COMP_CWORD} -eq 1 ]]; then
        _vlink_bash_complete_words "run host validate exec plugin status events start stop restart reload params set-param shutdown help -h --help -v --version" "$cur"
        return
    fi

    case "$prev" in
        --scheme)
            _vlink_bash_complete_words "dds ddsc ddsr zenoh intra" "$cur"
            return
            ;;
    esac

    case "$subcmd" in
        run|host)
            if [[ "$cur" == -* ]]; then
                _vlink_bash_complete_words "--arg --only --skip --dry-run --scheme --ns --domain --key --no-ctrl --no-respawn -h --help" "$cur"
            else
                _vlink_bash_complete_files_ext "$cur" "toml"
            fi
            ;;
        validate)
            if [[ "$cur" == -* ]]; then
                _vlink_bash_complete_words "--arg --check-programs -h --help" "$cur"
            else
                _vlink_bash_complete_files_ext "$cur" "toml"
            fi
            ;;
        exec)
            if [[ "$cur" == -* ]]; then
                _vlink_bash_complete_words "--name -n --ns --param -p --remap -r --scheduler --threads --queue --queue-strategy -h --help" "$cur"
            else
                _vlink_bash_complete_files_ext "$cur" "so|dylib|dll"
            fi
            ;;
        plugin)
            if [[ "$cur" == -* ]]; then
                _vlink_bash_complete_words "-h --help" "$cur"
            else
                _vlink_bash_complete_files_ext "$cur" "so|dylib|dll"
            fi
            ;;
        status|events|start|stop|restart|reload|params|set-param|shutdown)
            _vlink_bash_complete_words "--scheme --ns --domain --key --timeout -h --help" "$cur"
            ;;
    esac
}

_vlink_bash_register_completion _vlink_launcher vlink-launcher launcher
