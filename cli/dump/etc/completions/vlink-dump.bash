# VLink bash completion for vlink-dump.

_vlink_dash_dump() {
    local cur="${COMP_WORDS[COMP_CWORD]}"
    local prev="${COMP_WORDS[COMP_CWORD-1]}"
    local active_variadic=""

    COMPREPLY=()

    for ((i = 1; i < COMP_CWORD; ++i)); do
        case "${COMP_WORDS[i]}" in
            --urls|--ignore_compress|--actions)
                active_variadic="${COMP_WORDS[i]}"
                ;;
            -*)
                active_variadic=""
                ;;
        esac
    done

    case "$prev" in
        -t|--type)
            _vlink_bash_complete_words "console text csv json bin jpg jpeg h264 h265 raw pcd slice scan" "$cur"
            return
            ;;
        -o|--out_dir|-d|--proto_dir|--fbs_dir)
            _vlink_bash_complete_dirs "$cur"
            return
            ;;
        -f|--bag_file)
            _vlink_bash_complete_files_ext "$cur" "$_vlink_bash_play_ext"
            return
            ;;
        --segments|--schema_config|--schema_plugin)
            _vlink_bash_complete_files "$cur"
            return
            ;;
        --urls|--ignore_compress)
            _vlink_bash_complete_url "$cur"
            return
            ;;
        --actions)
            _vlink_bash_complete_words "0 1 2 3 4 5 6 7 8" "$cur"
            return
            ;;
        -c|--condition|-m|--base_name|-b|--begin_time|-e|--end_time|-n|--count|--hz|-x|--expression|-w|--window|--event|--pre|--post|--event_state_max_age|--event_min_interval|--suffix|--manifest|--scan_output|--filter|--url_filter|--tag|--cache_size|--compress_level|--sample_step|--dropout_threshold|--plugin)
            return
            ;;
    esac

    if [[ "$cur" != -* ]]; then
        case "$active_variadic" in
            --urls|--ignore_compress)
                _vlink_bash_complete_url "$cur"
                return
                ;;
            --actions)
                _vlink_bash_complete_words "0 1 2 3 4 5 6 7 8" "$cur"
                return
                ;;
        esac
    fi

    if [[ "$cur" == -* ]]; then
        _vlink_bash_complete_words "-t --type -c --condition -o --out_dir -m --base_name \
-f --bag_file -b --begin_time -e --end_time -n --count --hz --native \
-d --proto_dir --fbs_dir -q --quiet -l --detail -x --expression \
-w --window --segments --event --pre --post --event_state_max_age --event_min_interval \
--suffix --compress --force --no_manifest --manifest --schema_config --schema_plugin --plugin \
--scan_output --filter --export_csv --urls --url_filter --black --actions --tag --wal_mode --cache_size \
--compress_level --ignore_compress --sample_step --dry_run --quality_check \
--dropout_threshold -h --help -v --version" "$cur"
        return
    fi

    _vlink_bash_complete_url "$cur"
}

complete -F _vlink_dash_dump vlink-dump dump
