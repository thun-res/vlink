# VLink bash completion for vlink-trigger.

_vlink_trigger() {
    local cur="${COMP_WORDS[COMP_CWORD]}"
    local prev="${COMP_WORDS[COMP_CWORD-1]}"
    local subcommands="daemon dump"
    local subcommand
    local last_option=""

    COMPREPLY=()
    subcommand=$(_vlink_bash_find_subcommand "$subcommands")

    if [[ -z "$subcommand" ]]; then
        _vlink_bash_complete_words "$subcommands -h --help -v --version" "$cur"
        return
    fi

    case "$subcommand" in
        daemon)
            case "$prev" in
                -c|--config)
                    _vlink_bash_complete_files_ext "$cur" "json"
                    return
                    ;;
                --bag_plugin|--trigger_plugin|--trigger_plugin_config)
                    return
                    ;;
            esac

            _vlink_bash_complete_words "-c --config -n --native --bag_plugin --trigger_plugin \
--trigger_plugin_config -h --help" "$cur"
            ;;
        dump)
            case "$prev" in
                -m|--method_url|-u|--urls)
                    _vlink_bash_complete_url "$cur"
                    return
                    ;;
                -o|--out_file)
                    _vlink_bash_complete_files "$cur"
                    return
                    ;;
                -r|--reason|-n|--name|--pre|--post|-i|--filter)
                    return
                    ;;
            esac

            if [[ "$cur" != -* ]]; then
                last_option=$(_vlink_bash_find_last_option)
                if [[ "$last_option" == "-u" || "$last_option" == "--urls" ]]; then
                    _vlink_bash_complete_url "$cur"
                    return
                fi
            fi

            _vlink_bash_complete_words "-m --method_url -o --out_file -r --reason -n --name --pre --post \
-u --urls -i --filter -k --black -h --help" "$cur"
            ;;
    esac
}

complete -F _vlink_trigger vlink-trigger trigger
