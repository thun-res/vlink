# VLink bash completion for vlink-efbs.

_vlink_efbs() {
    local cur="${COMP_WORDS[COMP_CWORD]}"
    local prev="${COMP_WORDS[COMP_CWORD-1]}"
    local subcommands="pub sub import"
    local subcommand

    COMPREPLY=()
    subcommand=$(_vlink_bash_find_subcommand "$subcommands")

    if [[ -z "$subcommand" ]]; then
        _vlink_bash_complete_words "$subcommands -h --help -v --version" "$cur"
        return
    fi

    if [[ "$subcommand" == "import" ]]; then
        if [[ "$cur" == -* ]]; then
            _vlink_bash_complete_words "-h --help" "$cur"
        else
            _vlink_bash_complete_dirs "$cur"
        fi
        return
    fi

    case "$prev" in
        -d|--fbs_dir)
            _vlink_bash_complete_dirs "$cur"
            return
            ;;
        --schema_plugin)
            _vlink_bash_complete_files_ext "$cur" "$_vlink_bash_plugin_ext"
            return
            ;;
        -f|--fbstxt_file)
            _vlink_bash_complete_files "$cur"
            return
            ;;
        -x|--encoding)
            _vlink_bash_complete_words "protobuf flatbuffers raw blob zerocopy" "$cur"
            return
            ;;
        -s|--ser_type|-i|--filter|-c|--fbstxt_content|-l|--interval|-m|--max_str_count|--rows|--columns|--times)
            return
            ;;
    esac

    if [[ "$prev" == "-t" && "$subcommand" == "pub" ]]; then
        return
    fi

    if [[ "$cur" == -* ]]; then
        if [[ "$subcommand" == "pub" ]]; then
            _vlink_bash_complete_words "-d --fbs_dir --schema_plugin -s --ser_type -x --encoding \
-n --native -f --fbstxt_file -c --fbstxt_content -t --times -l --interval -h --help" "$cur"
        else
            _vlink_bash_complete_words "-d --fbs_dir --schema_plugin -s --ser_type -x --encoding \
-i --filter -g --getter -k --black -n --native -m --max_str_count \
-e --print_enum_string -r --ignore_array -t --ignore_string \
-y --print_time_string -u --print_hex_string -o --ignore_default \
-p --use_long_repeated --rows --columns -h --help" "$cur"
        fi
        return
    fi

    _vlink_bash_complete_url "$cur"
}

complete -F _vlink_efbs vlink-efbs efbs
